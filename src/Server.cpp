#include "Server.h"
#include "BlockingLoop.h"
#include "EventLoop.h"
#include "Listener.h"
#include "Logging.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <system_error>
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

ou::http::Server::StartError sStartError(const ou::SocketError &error) { return { .step = std::string(error.step), .code = error.code }; }

// Regex and group names for a {name} pattern; any other pattern is already a regex
std::pair<std::string, std::vector<std::string>> sCompileSegments(const std::string &pattern) {
	const std::regex segment(R"(\{([A-Za-z_]\w*)\})");
	std::string regex;
	std::vector<std::string> names;
	const auto appendLiteral = [&regex](std::string_view text) {
		for (const char c : text) {
			if (std::string_view(R"(\^$.|?*+()[]{})").contains(c))
				regex += '\\';
			regex += c;
		}
	};

	std::string_view rest = pattern;
	for (auto it = std::sregex_iterator(pattern.begin(), pattern.end(), segment); it != std::sregex_iterator(); ++it) {
		appendLiteral(it->prefix().str());
		regex += "([^/]+)";
		names.push_back((*it)[1].str());
		rest = std::string_view(pattern).substr(it->position() + it->length());
	}

	if (names.empty())
		return { pattern, {} };
	appendLiteral(rest);
	return { std::move(regex), std::move(names) };
}

} // namespace

namespace ou::http {

Server::Server(Config config) : config_(std::move(config)), staticFiles_(std::bind_front(&Server::handleStaticFileRequest, this)) {
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

	try {
#ifndef DISABLE_HTTPS
		if (config_.https.enabled) {
			socketHandler_ = std::make_unique<SSLSocketHandler>(config_.https);
		} else
#endif
		{
			socketHandler_ = std::make_unique<PlainSocketHandler>();
		}
	} catch (const std::exception &e) {
		return std::unexpected(StartError{ .step = std::format("initialize socket handler: {}", e.what()), .code = {} });
	}

	std::expected<ou::Listener, StartError> listener = ou::openListener(config_.port).transform_error(sStartError);
	if (!listener)
		return std::unexpected(std::move(listener).error());

	port_ = listener->port;
	listenSocket_ = std::move(listener->socket);
	logInfo("Server socket {} bound and listening on port {}...", listenSocket_.get(), port_);

	if (config_.model == Model::EventLoop && config_.handlerThreads > 0)
		pool_.emplace(config_.handlerThreads);

	for (unsigned i = 0; i < config_.threadCount; ++i) {
		threads_.emplace_back([this](const std::stop_token &stop) { runLoop(stop); });
	}
	return {};
}

void Server::stop() {
	if (!listenSocket_ && threads_.empty())
		return;

	logInfo("Stopping server...");

	if (pool_)
		pool_->stop();
	for (std::jthread &thread : threads_)
		thread.request_stop();
	threads_.clear();
	listenSocket_.reset();

	logInfo("Server stopped");
}

void Server::addMiddleware(std::shared_ptr<Middleware> middleware) { middlewares_.push_back(std::move(middleware)); }

void Server::registerPathHandler(Method method, const std::string &path, AnyHandler handler) {
	pathHandlers_[{ method, path }] = std::move(handler.handler);
}

void Server::registerPatternHandler(Method method, const std::string &pattern, AnyHandler handler) {
	auto [regex, names] = sCompileSegments(pattern);
	patternHandlers_.push_back(
			{ .method = method, .pattern = std::regex(regex), .names = std::move(names), .handler = std::move(handler.handler) });
}

void Server::registerPathHandler(std::initializer_list<Method> methods, const std::string &path, const AnyHandler &handler) {
	for (const Method method : methods) {
		registerPathHandler(method, path, handler.handler);
	}
}

void Server::registerPatternHandler(std::initializer_list<Method> methods, const std::string &pattern, const AnyHandler &handler) {
	for (const Method method : methods) {
		registerPatternHandler(method, pattern, handler.handler);
	}
}

void Server::runLoop(const std::stop_token &stop) {
	const LoopConfig config{ .listenSocket = listenSocket_.get(),
													 .maxRequestBytes = config_.maxRequestBytes,
													 .idleTimeout = config_.readTimeout };

	const Dispatch dispatch = [this](Request &request, const sockaddr_in &clientAddr) {
		return serve(request, clientAddr);
	};
	const Reject reject = [this](int status, const sockaddr_in &clientAddr) {
		return rejectRequest(status, clientAddr);
	};

	if (config_.model == Model::EventLoop) {
		EventLoop loop(config, *socketHandler_, dispatch, reject, pool_ ? &*pool_ : nullptr);
		loop.run(stop);
		return;
	}

	BlockingLoop loop(config, *socketHandler_, dispatch, reject);
	loop.run(stop);
}

Task<Response> Server::handle(Request &request) const {
	for (const auto &middleware : middlewares_) {
		if (std::optional<Response> response = middleware->process(request))
			return std::move(*response);
	}

	return handlerFor(request)(request);
}

const Server::Handler &Server::handlerFor(Request &request) const {
	if (const auto path = pathHandlers_.find({ request.method, request.path }); path != pathHandlers_.end())
		return path->second;

	std::smatch match;
	for (const PatternRoute &pattern : patternHandlers_) {
		if (pattern.method != request.method || !std::regex_match(request.path, match, pattern.pattern))
			continue;

		for (size_t i = 0; i < match.size(); ++i)
			request.pathParams.emplace_back(match.position(i), match.length(i));
		request.pathParamNames = pattern.names;
		return pattern.handler;
	}

	return staticFiles_;
}

Response Server::postProcess(const Request &request, Response response) const {
	for (const auto &middleware : middlewares_) {
		middleware->postProcess(request, response);
	}

	if (request.method == Method::HEAD)
		response.body.clear();

	return response;
}

Response Server::handleRequest(Request request) const {
	Task<Response> handled = handle(request);
	return postProcess(request, handled.result());
}

Task<Response> Server::serve(Request &request, const sockaddr_in &clientAddr) const {
	request.clientAddr = clientAddr;

	// Taken before handling; middlewares may rewrite the request
	const bool keepAlive = config_.keepAlive && request.wantsKeepAlive();
	std::string logLine
			= config_.logRequests ? std::format("{} {} from {}", request.method, request.path, addressToString(clientAddr)) : std::string();

	Task<Response> handled = handle(request);
	if (handled.done())
		return finalize(request, handled.result(), keepAlive, logLine);
	return finalizeLater(std::move(handled), request, keepAlive, std::move(logLine));
}

Response Server::finalize(const Request &request, Response response, bool keepAlive, std::string_view logLine) const {
	response = postProcess(request, std::move(response));
	response.keepAlive = keepAlive;

	if (config_.logRequests)
		logInfo("{} -> {}", logLine, response.statusCode);
	return response;
}

Task<Response> Server::finalizeLater(Task<Response> handled, const Request &request, bool keepAlive, std::string logLine) const {
	Response response = co_await handled;
	co_return finalize(request, std::move(response), keepAlive, logLine);
}

Response Server::rejectRequest(int status, const sockaddr_in &clientAddr) const {
	logWarn("Rejecting request from {} with status {}", addressToString(clientAddr), status);

	Request request;
	request.clientAddr = clientAddr;

	Response response = Response::error(status);
	for (const auto &middleware : middlewares_) {
		middleware->postProcess(request, response);
	}

	return response;
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
