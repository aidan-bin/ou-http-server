#include "Server.h"
#include "BlockingLoop.h"
#include "EventLoop.h"
#include "Logging.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>

namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 14> kMimeTypes{ {
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
} };

std::string_view sMimeTypeFor(const std::filesystem::path &path) {
	const std::string extension = path.extension().string();
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

Server::Server(Config config) : config_(std::move(config)) {
	std::error_code ec;
	servingRoot_ = std::filesystem::weakly_canonical(config_.servingDirectory, ec);
	if (ec) {
		LOG_ERROR("Cannot resolve serving directory: {}", config_.servingDirectory.string());
		servingRoot_.clear();
	}
}

Server::~Server() { stop(); }

bool Server::start() {
	LOG_INFO("Starting server with {} threads...", config_.threadCount);

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
		LOG_ERROR("Failed to initialize socket handler: {}", e.what());
		return false;
	}

	UniqueFd serverSocket(socket(AF_INET, SOCK_STREAM, 0));
	if (!serverSocket) {
		LOG_ERROR("Failed to create socket");
		return false;
	}

	// Lets a restarted server rebind while the old socket's connections sit in TIME_WAIT
	const int enable = 1;
	if (setsockopt(serverSocket.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
		LOG_ERROR("Failed to set SO_REUSEADDR");
		return false;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(config_.port);

	if (bind(serverSocket.get(), reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
		LOG_ERROR("Failed to bind socket");
		return false;
	}

	if (listen(serverSocket.get(), SOMAXCONN) < 0) {
		LOG_ERROR("Failed to listen on socket");
		return false;
	}

	// Non-blocking so the accept loop can poll and notice stop() without being closed out from under it
	if (!setNonBlocking(serverSocket.get(), true)) {
		LOG_ERROR("Failed to set socket non-blocking");
		return false;
	}

	sockaddr_in bound{};
	socklen_t boundLength = sizeof(bound);
	if (getsockname(serverSocket.get(), reinterpret_cast<struct sockaddr *>(&bound), &boundLength) < 0) {
		LOG_ERROR("Failed to read the bound port");
		return false;
	}
	port_ = ntohs(bound.sin_port);

	LOG_INFO("Server socket {} bound and listening on port {}...", serverSocket.get(), port_);
	listenSocket_ = std::move(serverSocket);

	running_.store(true);
	for (unsigned i = 0; i < config_.threadCount; ++i) {
		threads_.emplace_back([this]() { runLoop(); });
	}
	return true;
}

void Server::stop() {
	running_.store(false);
	if (!listenSocket_ && threads_.empty())
		return;

	LOG_INFO("Stopping server...");

	for (std::thread &thread : threads_) {
		if (thread.joinable())
			thread.join();
	}
	threads_.clear();
	listenSocket_.reset();

	LOG_INFO("Server stopped");
}

void Server::addMiddleware(std::shared_ptr<Middleware> middleware) { middlewares_.push_back(std::move(middleware)); }

void Server::registerPathHandler(Method method, const std::string &path, Handler handler) {
	pathHandlers_[{ method, path }] = std::move(handler);
}

void Server::registerPatternHandler(Method method, const std::string &pattern, Handler handler) {
	patternHandlers_.push_back({ .method = method, .pattern = std::regex(pattern), .handler = std::move(handler) });
}

void Server::registerPathHandler(std::initializer_list<Method> methods, const std::string &path, const Handler &handler) {
	for (const Method method : methods) {
		registerPathHandler(method, path, handler);
	}
}

void Server::registerPatternHandler(std::initializer_list<Method> methods, const std::string &pattern, const Handler &handler) {
	for (const Method method : methods) {
		registerPatternHandler(method, pattern, handler);
	}
}

void Server::runLoop() {
	const LoopConfig config{ .listenSocket = listenSocket_.get(),
													 .maxRequestBytes = config_.maxRequestBytes,
													 .idleTimeout = config_.readTimeout };

	const Dispatch dispatch = [this](Request request, const sockaddr_in &clientAddr) {
		return serve(std::move(request), clientAddr);
	};
	const Reject reject = [this](int status, const sockaddr_in &clientAddr) {
		return rejectRequest(status, clientAddr);
	};

	if (config_.model == Model::EventLoop) {
		EventLoop loop(config, *socketHandler_, dispatch, reject);
		loop.run(running_);
		return;
	}

	BlockingLoop loop(config, *socketHandler_, dispatch, reject);
	loop.run(running_);
}

Response Server::handleRequest(Request request) const {
	Response response;

	const auto dispatch = [&]() -> Response {
		for (const auto &middleware : middlewares_) {
			if (middleware->process(request, response))
				return response;
		}

		const auto pathHandler = pathHandlers_.find({ request.method, request.path });
		if (pathHandler != pathHandlers_.end())
			return pathHandler->second(request);

		for (const PatternRoute &route : patternHandlers_) {
			if (route.method == request.method && std::regex_match(request.path, route.pattern))
				return route.handler(request);
		}

		return handleStaticFileRequest(request);
	};

	response = dispatch();

	for (const auto &middleware : middlewares_) {
		middleware->postProcess(request, response);
	}

	if (request.method == Method::HEAD)
		response.body.clear();

	return response;
}

Response Server::serve(Request request, const sockaddr_in &clientAddr) const {
	request.clientAddr = clientAddr;
	const bool keepAlive = config_.keepAlive && request.wantsKeepAlive();

	const std::string requestLine = std::format("{} {}", request.method, request.path);
	Response response = handleRequest(std::move(request));
	response.keepAlive = keepAlive;

	LOG_INFO("{} from {} -> {}", requestLine, addressToString(clientAddr), response.statusCode);
	return response;
}

Response Server::rejectRequest(int status, const sockaddr_in &clientAddr) const {
	LOG_WARN("Rejecting request from {} with status {}", addressToString(clientAddr), status);

	Request request;
	request.clientAddr = clientAddr;

	Response response = Response::error(status);
	for (const auto &middleware : middlewares_) {
		middleware->postProcess(request, response);
	}

	return response;
}

Response Server::handleStaticFileRequest(const Request &request) const {
	if (!request.path.starts_with('/'))
		return Response::error(400);

	if (servingRoot_.empty())
		return Response::error(500);

	std::error_code ec;
	const std::filesystem::path target = std::filesystem::weakly_canonical(servingRoot_ / std::string_view(request.path).substr(1), ec);
	if (ec)
		return Response::error(404);

	if (!sIsWithin(target, servingRoot_)) {
		LOG_WARN("Rejected path outside serving directory: {}", request.path);
		return Response::error(403);
	}

	const std::filesystem::file_status targetStatus = std::filesystem::status(target, ec);
	if (ec || !std::filesystem::exists(targetStatus)) {
		LOG_ERROR("File not found: {}", target.string());
		return Response::error(404);
	}

	if (std::filesystem::is_directory(targetStatus)) {
		if (!config_.enableDirectoryIndexing) {
			LOG_WARN("Directory indexing is disabled, returning 403 Forbidden.");
			return Response::error(403);
		}

		std::string indexHtml = sGenerateDirectoryIndex(request.path, target, ec);
		if (ec) {
			LOG_ERROR("Failed to list directory: {}", target.string());
			return Response::error(500);
		}

		Response response;
		response.headers.emplace("Content-Type", "text/html");
		response.body = std::move(indexHtml);
		return response;
	}

	std::ifstream file(target, std::ios::binary);
	if (!file) {
		LOG_ERROR("Failed to open file: {}", target.string());
		return Response::error(500);
	}

	std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (file.bad()) {
		LOG_ERROR("Failed to read file: {}", target.string());
		return Response::error(500);
	}

	Response response;
	response.headers.emplace("Content-Type", std::string(sMimeTypeFor(target)));
	response.body = std::move(body);
	return response;
}

} // namespace ou::http
