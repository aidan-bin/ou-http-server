#include "Server.h"
#include "Logging.h"

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::string sGenerateDirectoryIndex(const std::string &requestPath, const std::filesystem::path &dirPath) {
	std::stringstream ss;
	ss << "<!DOCTYPE html><html><head><title>Index of " << requestPath << "</title></head><body>";
	ss << "<h1>Index of " << requestPath << "</h1><ul>";

	std::string normalizedPath = requestPath;
	if (normalizedPath.back() != '/')
		normalizedPath += '/';

	if (requestPath != "/") {
		std::string parentPath = normalizedPath.substr(0, normalizedPath.find_last_of('/', normalizedPath.length() - 2) + 1);
		ss << "<li><a href=\"" << parentPath << "\">..</a></li>";
	}

	for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
		std::string name = entry.path().filename().string();
		std::string href = normalizedPath + name;
		if (entry.is_directory())
			href += "/";

		ss << "<li><a href=\"" << href << "\">" << name << (entry.is_directory() ? "/" : "") << "</a></li>";
	}

	ss << "</ul></body></html>";
	return ss.str();
}

} // namespace

namespace ou::http {

Server::Server(Config config) : config_(std::move(config)) {
#ifndef DISABLE_HTTPS
	if (config_.https.enabled) {
		socketHandler_ = std::make_unique<SSLSocketHandler>(config_.https);
	} else
#endif
	{
		socketHandler_ = std::make_unique<PlainSocketHandler>();
	}
}

Server::~Server() { stop(); }

bool Server::init() {
	LOG_INFO("Initializing server on port {} with {} threads...", config_.port, config_.threadCount);

	for (int i = 0; i < config_.threadCount; ++i) {
		int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
		if (serverSocket < 0) {
			LOG_ERROR("Failed to create socket");
			return false;
		}

		int opt = 1;
		if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
			LOG_ERROR("Failed to set SO_REUSEPORT");
			close(serverSocket);
			return false;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(config_.port);

		if (bind(serverSocket, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
			LOG_ERROR("Failed to bind socket");
			close(serverSocket);
			return false;
		}

		if (listen(serverSocket, SOMAXCONN) < 0) {
			LOG_ERROR("Failed to listen on socket");
			close(serverSocket);
			return false;
		}

		LOG_INFO("Server socket {} bound and listening...", serverSocket);
		sockets_.push_back(serverSocket);
	}

	return true;
}

void Server::start() {
	LOG_INFO("Starting server...");
	running_.store(true);
	for (int socket : sockets_) {
		threads_.emplace_back([this, socket]() { workerThread(socket); });
	}
}

void Server::stop() {
	LOG_INFO("Stopping server...");
	running_.store(false);
	for (int socket : sockets_) {
		close(socket);
		LOG_INFO("Closed socket {}", socket);
	}
	for (auto &thread : threads_) {
		if (thread.joinable()) {
			thread.join();
		}
	}
	LOG_INFO("Server stopped");
}

void Server::addMiddleware(std::shared_ptr<Middleware> middleware) { middlewares_.push_back(std::move(middleware)); }

void Server::registerPathHandler(Method method, const std::string &path, const std::shared_ptr<RequestHandler> &handler) {
	routeHandlers_[method][path] = [handler](const Request &req) {
		return handler->handle(req);
	};
}

void Server::registerPathHandler(Method method, const std::string &path, std::function<Response(const Request &)> handler) {
	routeHandlers_[method][path] = std::move(handler);
}

void Server::registerPatternHandler(Method method, const std::string &pattern, const std::shared_ptr<RequestHandler> &handler) {
	patternHandlers_[method].emplace_back(std::regex(pattern), [handler](const Request &req) { return handler->handle(req); });
}

void Server::registerPatternHandler(Method method, const std::string &pattern, std::function<Response(const Request &)> handler) {
	patternHandlers_[method].emplace_back(std::regex(pattern), std::move(handler));
}

void Server::registerPathHandler(const std::set<Method> &methods, const std::string &path, const std::shared_ptr<RequestHandler> &handler) {
	for (const auto &method : methods) {
		registerPathHandler(method, path, handler);
	}
}

void Server::registerPathHandler(const std::set<Method> &methods, const std::string &path,
																 const std::function<Response(const Request &)> &handler) {
	for (const auto &method : methods) {
		registerPathHandler(method, path, handler);
	}
}

void Server::registerPatternHandler(const std::set<Method> &methods, const std::string &pattern,
																		const std::shared_ptr<RequestHandler> &handler) {
	for (const auto &method : methods) {
		registerPatternHandler(method, pattern, handler);
	}
}

void Server::registerPatternHandler(const std::set<Method> &methods, const std::string &pattern,
																		const std::function<Response(const Request &)> &handler) {
	for (const auto &method : methods) {
		registerPatternHandler(method, pattern, handler);
	}
}

void Server::workerThread(int serverSocket) {
	while (running_.load()) {
		sockaddr_in clientAddr{};
		socklen_t clientLen = sizeof(clientAddr);
		int clientSocket = accept(serverSocket, reinterpret_cast<struct sockaddr *>(&clientAddr), &clientLen);
		if (clientSocket < 0) {
			if (!running_)
				break;
			continue;
		}

		LOG_INFO("Accepted connection from {}", inet_ntoa(clientAddr.sin_addr));

		if (!socketHandler_->acceptConnection(clientSocket)) {
			close(clientSocket);
			continue;
		}

		std::array<char, 4096> buffer{};
		ssize_t bytesRead = socketHandler_->read(clientSocket, buffer.data(), buffer.size());

		if (bytesRead <= 0) {
			LOG_WARN("Failed to read request from client {}", inet_ntoa(clientAddr.sin_addr));
			socketHandler_->closeConnection(clientSocket);
			continue;
		}

		Request request = Request::parse(std::string_view(buffer.data(), bytesRead));
		LOG_INFO("Received request: {} {}", request.method, request.path);

		request.clientAddr = clientAddr;

		auto response = handleRequest(request);

		if (response) {
			std::string responseStr = response->serialize();
			LOG_INFO("Sending response: {} {}", response->statusCode, response->reasonPhrase);
			socketHandler_->write(clientSocket, responseStr);
		} else {
			LOG_WARN("No response generated for request: {} {}", request.method, request.path);
		}

		socketHandler_->closeConnection(clientSocket);
		LOG_INFO("Closed connection from {}", inet_ntoa(clientAddr.sin_addr));
	}
}

std::optional<Response> Server::handleRequest(const Request &request) const {
	Request processedRequest = request;
	Response response;
	bool handled = false;

	for (const auto &middleware : middlewares_) {
		if (middleware->process(processedRequest, response)) {
			handled = true;
			break;
		}
	}

	if (handled)
		return response;

	auto methodIt = routeHandlers_.find(request.method);
	if (methodIt != routeHandlers_.end()) {
		auto handlerIt = methodIt->second.find(request.path);
		if (handlerIt != methodIt->second.end()) {
			return handlerIt->second(processedRequest);
		}
	}

	auto patternIt = patternHandlers_.find(request.method);
	if (patternIt != patternHandlers_.end()) {
		for (const auto &[pattern, handler] : patternIt->second) {
			if (std::regex_match(request.path, pattern)) {
				return handler(processedRequest);
			}
		}
	}

	return handleStaticFileRequest(processedRequest);
}

std::optional<Response> Server::handleStaticFileRequest(const Request &request) const {
	std::filesystem::path filePath = config_.servingDirectory / (request.path == "/" ? "" : request.path.substr(1));

	LOG_INFO("Handling request for path: {}", request.path);

	if (!std::filesystem::exists(filePath)) {
		LOG_ERROR("File not found: {}", filePath.string());
		return Response{ 404, "Not Found", { { "Content-Type", "text/plain" } }, "404 Not Found" };
	}

	if (std::filesystem::is_directory(filePath)) {
		LOG_INFO("Request is a directory: {}", filePath.string());
		if (!config_.enableDirectoryIndexing) {
			LOG_WARN("Directory indexing is disabled, returning 403 Forbidden.");
			return Response{ 403, "Forbidden", { { "Content-Type", "text/plain" } }, "403 Forbidden" };
		}

		std::string indexHtml = sGenerateDirectoryIndex(request.path, filePath);
		LOG_INFO("Generated directory index for {}", request.path);
		return Response{ 200, "OK", { { "Content-Type", "text/html" }, { "Content-Length", std::to_string(indexHtml.size()) } }, indexHtml };
	}

	std::ifstream file(filePath, std::ios::binary);
	if (!file) {
		LOG_ERROR("Failed to open file: {}", filePath.string());
		return Response{ 500, "Internal Server Error", { { "Content-Type", "text/plain" } }, "500 Internal Server Error" };
	}

	std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	LOG_INFO("Successfully read file: {}", filePath.string());
	return Response{ 200, "OK", { { "Content-Length", std::to_string(body.size()) }, { "Content-Type", "text/plain" } }, body };
}

} // namespace ou::http
