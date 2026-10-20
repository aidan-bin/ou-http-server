#include "ou/http/Server.h"
#include "Transport.h"
#include "UniqueFd.h"
#include "ou/http/Logging.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr auto kMimeTypes = std::to_array<std::pair<std::string_view, std::string_view>>({
		{ ".html", "text/html" },
		{ ".htm", "text/html" },
		{ ".css", "text/css" },
		{ ".js", "text/javascript" },
		{ ".json", "application/json" },
		{ ".txt", "text/plain" },
		{ ".xml", "application/xml" },
		{ ".svg", "image/svg+xml" },
		{ ".png", "image/png" },
		{ ".jpg", "image/jpeg" },
		{ ".jpeg", "image/jpeg" },
		{ ".gif", "image/gif" },
		{ ".ico", "image/vnd.microsoft.icon" },
		{ ".wasm", "application/wasm" },
});

// Finds the filename's extension as path::extension() does; empty for a dotfile like ".html"
std::string_view sExtension(std::string_view path) {
	const size_t dot = path.find_last_of("./");
	if (dot == std::string_view::npos || path[dot] != '.' || dot == 0 || path[dot - 1] == '/')
		return {};
	return path.substr(dot);
}

std::string_view sMimeTypeFor(std::string_view path) {
	const std::string_view extension = sExtension(path);
	for (const auto &[suffix, type] : kMimeTypes) {
		if (ou::http::equalsIgnoreCase(extension, suffix))
			return type;
	}
	return "application/octet-stream";
}

// Compares whole path components (e.g., so /srv/www2 is not inside /srv/www)
bool sIsWithin(const std::filesystem::path &candidate, const std::filesystem::path &root) {
	const auto mismatched = std::ranges::mismatch(root, candidate);
	return mismatched.in1 == root.end();
}

ou::UniqueFd sOpenReadOnly(const std::string &path) {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) open() is variadic
	return ou::UniqueFd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
}

// Applies "." and ".." like lexically_normal(), without its per-segment allocations; null if ".." climbs above the root
std::optional<std::string> sNormalizePath(std::string_view path) {
	std::string normalized;
	for (const auto part : std::views::split(path, '/')) {
		const std::string_view segment(part.begin(), part.end());
		if (segment.empty() || segment == ".")
			continue;

		if (segment == "..") {
			if (normalized.empty())
				return std::nullopt;
			normalized.resize(normalized.rfind('/'));
			continue;
		}

		normalized += '/';
		normalized += segment;
	}
	return normalized;
}

// Reads up to size bytes
std::optional<std::string> sReadFile(int fd, size_t size) {
	std::string body(size, '\0');
	size_t filled = 0;
	while (filled < size) {
		const std::span<char> rest = std::span(body).subspan(filled);
		const ssize_t bytes = ::read(fd, rest.data(), rest.size());
		if (bytes < 0)
			return std::nullopt;
		if (bytes == 0)
			break;
		filled += static_cast<size_t>(bytes);
	}
	body.resize(filled);
	return body;
}

std::string sHtmlEscape(std::string_view text) {
	std::string escaped;
	escaped.reserve(text.size());

	for (const char ch : text) {
		switch (ch) {
		case '&':
			escaped += "&amp;";
			break;
		case '<':
			escaped += "&lt;";
			break;
		case '>':
			escaped += "&gt;";
			break;
		case '"':
			escaped += "&quot;";
			break;
		case '\'':
			escaped += "&#39;";
			break;
		default:
			escaped += ch;
			break;
		}
	}

	return escaped;
}

// A bare <ul> of links for a directory's contents, with a link to the parent if not at the root
std::string sGenerateDirectoryIndex(const std::string &requestPath, const std::filesystem::path &dirPath, std::error_code &ec) {
	const std::string escapedPath = sHtmlEscape(requestPath);
	std::string html
			= std::format("<!DOCTYPE html><html><head><title>Index of {0}</title></head><body><h1>Index of {0}</h1><ul>", escapedPath);

	std::string normalizedPath = requestPath;
	if (!normalizedPath.ends_with('/'))
		normalizedPath += '/';

	if (requestPath != "/") {
		// "/a/b/" -> "/a/"
		const std::string parentPath = normalizedPath.substr(0, normalizedPath.find_last_of('/', normalizedPath.length() - 2) + 1);
		html += std::format("<li><a href=\"{}\">..</a></li>", ou::http::percentEncode(parentPath));
	}

	for (const auto &entry : std::filesystem::directory_iterator(dirPath, ec)) {
		const std::string name = entry.path().filename().string();
		const std::string_view slash = entry.is_directory(ec) ? "/" : "";

		html += std::format("<li><a href=\"{}{}\">{}{}</a></li>", ou::http::percentEncode(normalizedPath + name), slash, sHtmlEscape(name),
												slash);
	}

	html += "</ul></body></html>";
	return html;
}

} // namespace

namespace ou::http {

Server::Server(Config config, Router router) : config_(std::move(config)), router_(std::move(router)) {
	if (!router_.hasFallback())
		router_.fallback(std::bind_front(&Server::handleStaticFileRequest, this));

	std::error_code ec;
	servingRoot_ = std::filesystem::weakly_canonical(config_.servingDirectory, ec);
	if (!ec && config_.preloadBytes > 0)
		preload();
}

void Server::preload() {
	size_t total = 0;
	size_t skipped = 0;
	std::error_code ec;

	for (auto it = std::filesystem::recursive_directory_iterator(servingRoot_, ec);
			 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
		if (!it->is_regular_file(ec))
			continue;

		const std::filesystem::path target = std::filesystem::canonical(it->path(), ec);
		if (ec || !sIsWithin(target, servingRoot_))
			continue;

		const size_t size = it->file_size(ec);
		if (ec || total + size > config_.preloadBytes) {
			++skipped;
			continue;
		}

		const UniqueFd file = sOpenReadOnly(target.native());
		std::optional<std::string> body = file ? sReadFile(file.get(), size) : std::nullopt;
		if (!body)
			continue;

		total += body->size();
		const std::string requestPath = "/" + it->path().lexically_relative(servingRoot_).generic_string();
		preloaded_.emplace(requestPath, PreloadedFile{ .body = std::move(*body), .contentType = sMimeTypeFor(target.native()) });
	}

	logInfo("Preloaded {} files ({} bytes)", preloaded_.size(), total);
	if (skipped > 0)
		logWarn("{} files did not fit in preloadBytes and will be served from disk", skipped);
}

Server::~Server() { stop(); }

std::expected<void, Server::StartError> Server::start() {
	logInfo("Starting server with {} threads...", config_.threadCount);

	std::error_code ec;
	if (!std::filesystem::is_directory(servingRoot_, ec)) {
		return std::unexpected(StartError{ .step = std::format("open serving directory {}", config_.servingDirectory.string()),
																			 .code = ec ? ec : std::make_error_code(std::errc::not_a_directory) });
	}

	auto transport = Transport::start(
			config_, [this](Request &request, const sockaddr_in &clientAddr) { return serve(request, clientAddr); },
			[this](int status, const sockaddr_in &clientAddr) { return rejectRequest(status, clientAddr); });
	if (!transport)
		return std::unexpected(std::move(transport).error());

	transport_ = std::move(*transport);
	port_ = transport_->port();
	return {};
}

void Server::stop() {
	if (!transport_)
		return;

	logInfo("Stopping server...");
	transport_.reset();

	logInfo("Server stopped");
}

Task<Response> Server::serve(Request &request, const sockaddr_in &clientAddr) const {
	request.clientAddr = clientAddr;

	// Taken before handling; middlewares may rewrite the request
	const bool keepAlive = config_.keepAlive && request.wantsKeepAlive();
	std::string logLine
			= config_.logRequests ? std::format("{} {} from {}", request.method, request.path, addressToString(clientAddr)) : std::string();

	Task<Response> handled = router_.handle(request);
	if (handled.done())
		return finalize(handled.result(), keepAlive, logLine);
	return finalizeLater(std::move(handled), keepAlive, std::move(logLine));
}

Response Server::finalize(Response response, bool keepAlive, std::string_view logLine) const {
	response.keepAlive = keepAlive;

	if (config_.logRequests)
		logInfo("{} -> {}", logLine, response.statusCode);
	return response;
}

Task<Response> Server::finalizeLater(Task<Response> handled, bool keepAlive, std::string logLine) const {
	Response response = co_await handled;
	co_return finalize(std::move(response), keepAlive, logLine);
}

Response Server::rejectRequest(int status, const sockaddr_in &clientAddr) const {
	logWarn("Rejecting request from {} with status {}", addressToString(clientAddr), status);

	Request request;
	request.clientAddr = clientAddr;

	return router_.postProcess(request, Response::error(status));
}

std::expected<std::string, int> Server::resolveTarget(const std::string &requestPath) const {
	if (config_.skipSymlinkCheck) {
		const std::optional<std::string> normalized = sNormalizePath(requestPath);
		if (normalized)
			return servingRoot_.native() + *normalized;
	} else {
		std::error_code ec;
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(servingRoot_ / std::string_view(requestPath).substr(1), ec);
		if (ec)
			return std::unexpected(404);
		if (sIsWithin(canonical, servingRoot_))
			return canonical.native();
	}

	logWarn("Rejected path outside serving directory: {}", requestPath);
	return std::unexpected(403);
}

Response Server::handleStaticFileRequest(const Request &request) const {
	if (!request.path.starts_with('/'))
		return Response::error(400);

	if (servingRoot_.empty())
		return Response::error(500);

	if (const auto preloaded = preloaded_.find(request.path); preloaded != preloaded_.end()) {
		Response response;
		response.headers.emplace("Content-Type", preloaded->second.contentType);
		response.body = preloaded->second.body;
		return response;
	}

	const std::expected<std::string, int> target = resolveTarget(request.path);
	if (!target)
		return Response::error(target.error());

	const UniqueFd file = sOpenReadOnly(*target);
	if (!file && (errno == ENOENT || errno == ENOTDIR)) {
		logError("File not found: {}", *target);
		return Response::error(404);
	}

	struct stat targetStatus{};
	if (!file || ::fstat(file.get(), &targetStatus) < 0) {
		logError("Failed to open file: {}", *target);
		return Response::error(500);
	}

	if (S_ISDIR(targetStatus.st_mode)) {
		if (!config_.enableDirectoryIndexing) {
			logWarn("Directory indexing is disabled, returning 403 Forbidden");
			return Response::error(403);
		}

		std::error_code ec;
		std::string indexHtml = sGenerateDirectoryIndex(request.path, *target, ec);
		if (ec) {
			logError("Failed to list directory: {}", *target);
			return Response::error(500);
		}

		Response response;
		response.headers.emplace("Content-Type", "text/html");
		response.body = std::move(indexHtml);
		return response;
	}

	std::optional<std::string> body = sReadFile(file.get(), static_cast<size_t>(targetStatus.st_size));
	if (!body) {
		logError("Failed to read file: {}", *target);
		return Response::error(500);
	}

	Response response;
	response.headers.emplace("Content-Type", std::string(sMimeTypeFor(*target)));
	response.body = std::move(*body);
	return response;
}

} // namespace ou::http
