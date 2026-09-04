#include "Server.h"
#include "Logging.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr size_t kReadChunkBytes = 4096;

constexpr int kAcceptPollMs = 200;
constexpr auto kAcceptRetryDelay = std::chrono::milliseconds(10);

bool sSetNonBlocking(int fd, bool enable) {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
	const int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return false;

	const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
	return ::fcntl(fd, F_SETFL, updated) >= 0;
}

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

std::string sGenerateDirectoryIndex(const std::string &requestPath, const std::filesystem::path &dirPath, std::error_code &ec) {
	const std::string escapedPath = sHtmlEscape(requestPath);
	std::string html
			= std::format("<!DOCTYPE html><html><head><title>Index of {0}</title></head><body><h1>Index of {0}</h1><ul>", escapedPath);

	std::string normalizedPath = requestPath;
	if (!normalizedPath.ends_with('/'))
		normalizedPath += '/';

	if (requestPath != "/") {
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

int sAcceptOne(int serverSocket, sockaddr_in &clientAddr) {
	pollfd descriptor{ .fd = serverSocket, .events = POLLIN, .revents = 0 };

	const int ready = ::poll(&descriptor, 1, kAcceptPollMs);
	if (ready == 0) {
		return -1;
	}

	if (ready < 0) {
		if (errno != EINTR)
			std::this_thread::sleep_for(kAcceptRetryDelay);
		return -1;
	}

	socklen_t clientLen = sizeof(clientAddr);
	const int clientSocket = accept(serverSocket, reinterpret_cast<struct sockaddr *>(&clientAddr), &clientLen);
	if (clientSocket < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
			return -1;

		LOG_WARN("Failed to accept connection: errno {}", errno);
		std::this_thread::sleep_for(kAcceptRetryDelay);
		return -1;
	}

	(void)sSetNonBlocking(clientSocket, false);
	return clientSocket;
}

} // namespace

namespace ou::http {

Server::Server(Config config) : config_(std::move(config)) {}

Server::~Server() { stop(); }

bool Server::init() {
	LOG_INFO("Initializing server on port {} with {} threads...", config_.port, config_.threadCount);

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

	std::vector<UniqueFd> sockets;
	sockets.reserve(config_.threadCount);

	for (unsigned i = 0; i < config_.threadCount; ++i) {
		UniqueFd serverSocket(socket(AF_INET, SOCK_STREAM, 0));
		if (!serverSocket) {
			LOG_ERROR("Failed to create socket");
			return false;
		}

		const int opt = 1;
		if (setsockopt(serverSocket.get(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
			LOG_ERROR("Failed to set SO_REUSEPORT");
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
		if (!sSetNonBlocking(serverSocket.get(), true)) {
			LOG_ERROR("Failed to set socket non-blocking");
			return false;
		}

		LOG_INFO("Server socket {} bound and listening...", serverSocket.get());
		sockets.push_back(std::move(serverSocket));
	}

	sockets_ = std::move(sockets);
	return true;
}

void Server::start() {
	LOG_INFO("Starting server...");
	running_.store(true);
	for (const auto &socket : sockets_) {
		threads_.emplace_back([this, fd = socket.get()]() { workerThread(fd); });
	}
}

void Server::stop() {
	running_.store(false);
	if (sockets_.empty() && threads_.empty())
		return;

	LOG_INFO("Stopping server...");

	for (std::thread &thread : threads_) {
		if (thread.joinable())
			thread.join();
	}
	threads_.clear();
	sockets_.clear();

	LOG_INFO("Server stopped");
}

void Server::addMiddleware(std::shared_ptr<Middleware> middleware) { middlewares_.push_back(std::move(middleware)); }

void Server::registerPathHandler(Method method, const std::string &path, std::shared_ptr<RequestHandler> handler) {
	registerPathHandler(method, path, [handler = std::move(handler)](const Request &request) { return handler->handle(request); });
}

void Server::registerPathHandler(Method method, const std::string &path, Handler handler) {
	routeHandlers_[method][path] = std::move(handler);
}

void Server::registerPatternHandler(Method method, const std::string &pattern, std::shared_ptr<RequestHandler> handler) {
	registerPatternHandler(method, pattern, [handler = std::move(handler)](const Request &request) { return handler->handle(request); });
}

void Server::registerPatternHandler(Method method, const std::string &pattern, Handler handler) {
	patternHandlers_[method].emplace_back(std::regex(pattern), std::move(handler));
}

void Server::registerPathHandler(std::initializer_list<Method> methods, const std::string &path,
																 const std::shared_ptr<RequestHandler> &handler) {
	for (const Method method : methods) {
		registerPathHandler(method, path, handler);
	}
}

void Server::registerPathHandler(std::initializer_list<Method> methods, const std::string &path, const Handler &handler) {
	for (const Method method : methods) {
		registerPathHandler(method, path, handler);
	}
}

void Server::registerPatternHandler(std::initializer_list<Method> methods, const std::string &pattern,
																		const std::shared_ptr<RequestHandler> &handler) {
	for (const Method method : methods) {
		registerPatternHandler(method, pattern, handler);
	}
}

void Server::registerPatternHandler(std::initializer_list<Method> methods, const std::string &pattern, const Handler &handler) {
	for (const Method method : methods) {
		registerPatternHandler(method, pattern, handler);
	}
}

std::variant<Request, int> Server::readRequest(int clientSocket) const {
	std::string raw;
	std::array<char, kReadChunkBytes> chunk{};

	size_t headerEnd = std::string::npos;
	size_t totalSize = 0;
	std::optional<Request> request;

	while (!request || raw.size() < totalSize) {
		const ssize_t bytesRead = socketHandler_->read(clientSocket, chunk);
		if (bytesRead <= 0) {
			// Timed out, or the peer went away before finishing
			return raw.empty() ? 0 : 408;
		}

		raw.append(chunk.data(), static_cast<size_t>(bytesRead));
		if (raw.size() > config_.maxRequestBytes)
			return 413;

		if (headerEnd == std::string::npos) {
			headerEnd = raw.find("\r\n\r\n");
			if (headerEnd == std::string::npos)
				continue;
		}

		if (!request) {
			request = Request::parse(raw);
			if (!request)
				return 400;

			if (request->header("Transfer-Encoding"))
				return 501;

			totalSize = headerEnd + 4 + request->contentLength().value_or(0);
			if (totalSize > config_.maxRequestBytes)
				return 413;
		}
	}

	// Truncate any bytes past Content-Length that arrived in the same read
	const size_t bodyStart = headerEnd + 4;
	request->body.assign(raw, bodyStart, totalSize - bodyStart);

	return std::move(*request);
}

void Server::handleConnection(int clientSocket, const sockaddr_in &clientAddr) const {
	const timeval timeout{ .tv_sec = static_cast<time_t>(config_.readTimeout.count()), .tv_usec = 0 };
	(void)setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	if (!socketHandler_->acceptConnection(clientSocket))
		return;

	std::variant<Request, int> result = readRequest(clientSocket);

	if (const int *status = std::get_if<int>(&result)) {
		if (*status == 0)
			return;

		LOG_WARN("Rejecting request from {} with status {}", addressToString(clientAddr), *status);
		(void)socketHandler_->write(clientSocket, Response::error(*status).serialize());
		return;
	}

	Request request = std::move(std::get<Request>(result));
	request.clientAddr = clientAddr;

	const std::string requestLine = std::format("{} {}", request.method, request.path);
	const Response response = handleRequest(std::move(request));
	LOG_INFO("{} from {} -> {}", requestLine, addressToString(clientAddr), response.statusCode);
	(void)socketHandler_->write(clientSocket, response.serialize());
}

void Server::workerThread(int serverSocket) {
	while (running_.load()) {
		sockaddr_in clientAddr{};

		const int clientSocket = sAcceptOne(serverSocket, clientAddr);
		if (clientSocket < 0)
			continue;

		try {
			handleConnection(clientSocket, clientAddr);
		} catch (const std::exception &e) {
			LOG_ERROR("Unhandled exception while serving client: {}", e.what());
		} catch (...) {
			LOG_ERROR("Unhandled exception of unknown type while serving client");
		}

		socketHandler_->closeConnection(clientSocket);
	}
}

Response Server::handleRequest(Request request) const {
	Response response;

	const auto dispatch = [&]() -> Response {
		for (const auto &middleware : middlewares_) {
			if (middleware->process(request, response))
				return response;
		}

		const auto methodIt = routeHandlers_.find(request.method);
		if (methodIt != routeHandlers_.end()) {
			const auto handlerIt = methodIt->second.find(request.path);
			if (handlerIt != methodIt->second.end())
				return handlerIt->second(request);
		}

		const auto patternIt = patternHandlers_.find(request.method);
		if (patternIt != patternHandlers_.end()) {
			for (const auto &[pattern, handler] : patternIt->second) {
				if (std::regex_match(request.path, pattern))
					return handler(request);
			}
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

Response Server::handleStaticFileRequest(const Request &request) const {
	if (!request.path.starts_with('/'))
		return Response::error(400);

	std::error_code ec;
	const std::filesystem::path root = std::filesystem::weakly_canonical(config_.servingDirectory, ec);
	if (ec) {
		LOG_ERROR("Cannot resolve serving directory: {}", config_.servingDirectory.string());
		return Response::error(500);
	}

	const std::filesystem::path target = std::filesystem::weakly_canonical(root / std::string_view(request.path).substr(1), ec);
	if (ec)
		return Response::error(404);

	if (!sIsWithin(target, root)) {
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
