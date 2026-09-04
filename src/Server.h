#pragma once

#include "HttpTypes.h"
#include "SSLSocketHandler.h"
#include "SocketHandler.h"
#include "UniqueFd.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace ou::http {

class Middleware {
public:
	Middleware() = default;
	Middleware(const Middleware &) = delete;
	Middleware &operator=(const Middleware &) = delete;
	Middleware(Middleware &&) = delete;
	Middleware &operator=(Middleware &&) = delete;
	virtual ~Middleware() = default;

	// Return true if the middleware handled the request, optionally modify request, and set response
	virtual bool process(Request &request, Response &response) = 0;

	// Called after routing, with the final response
	virtual void postProcess(const Request & /*request*/, const Response & /*response*/) {}
};

class RequestHandler {
public:
	RequestHandler() = default;
	RequestHandler(const RequestHandler &) = delete;
	RequestHandler &operator=(const RequestHandler &) = delete;
	RequestHandler(RequestHandler &&) = delete;
	RequestHandler &operator=(RequestHandler &&) = delete;
	virtual ~RequestHandler() = default;

	virtual Response handle(const Request &request) = 0;
};

class Server {
public:
	using Handler = std::function<Response(const Request &)>;

	struct Config {
		std::filesystem::path servingDirectory;
		uint16_t port = 8080;
		unsigned threadCount = 4;
		bool enableDirectoryIndexing = false;

		// Requests over this limit are rejected with 413
		size_t maxRequestBytes = size_t{ 1024 } * 1024;
		// Stops a stalled connection from holding a worker thread
		std::chrono::seconds readTimeout{ 10 };

#ifndef DISABLE_HTTPS
		SSLSocketHandler::Config https;
#endif
	};

	explicit Server(Config config);
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;
	Server(Server &&) = delete;
	Server &operator=(Server &&) = delete;
	~Server();

	[[nodiscard]] bool init();
	void start();
	void stop();

	void addMiddleware(std::shared_ptr<Middleware> middleware);

	void registerPathHandler(Method method, const std::string &path, std::shared_ptr<RequestHandler> handler);
	void registerPathHandler(Method method, const std::string &path, Handler handler);
	void registerPatternHandler(Method method, const std::string &pattern, std::shared_ptr<RequestHandler> handler);
	void registerPatternHandler(Method method, const std::string &pattern, Handler handler);

	void registerPathHandler(std::initializer_list<Method> methods, const std::string &path, const std::shared_ptr<RequestHandler> &handler);
	void registerPathHandler(std::initializer_list<Method> methods, const std::string &path, const Handler &handler);
	void registerPatternHandler(std::initializer_list<Method> methods, const std::string &pattern,
															const std::shared_ptr<RequestHandler> &handler);
	void registerPatternHandler(std::initializer_list<Method> methods, const std::string &pattern, const Handler &handler);

protected:
	[[nodiscard]] Response handleRequest(const Request &request) const;
	[[nodiscard]] Response handleStaticFileRequest(const Request &request) const;

private:
	void workerThread(int serverSocket);

	// Never throws, and never closes the socket
	void handleConnection(int clientSocket, const sockaddr_in &clientAddr) const;

	// Reads one complete request, or the status to answer with instead (0 = close silently)
	[[nodiscard]] std::variant<Request, int> readRequest(int clientSocket) const;

	Config config_;
	std::atomic<bool> running_{ false };
	std::vector<std::thread> threads_;
	std::vector<UniqueFd> sockets_; // One per worker thread

	std::unordered_map<Method, std::unordered_map<std::string, Handler>> routeHandlers_;
	std::unordered_map<Method, std::vector<std::pair<std::regex, Handler>>> patternHandlers_;
	std::vector<std::shared_ptr<Middleware>> middlewares_;

	std::unique_ptr<SocketHandler> socketHandler_;
};

} // namespace ou::http
