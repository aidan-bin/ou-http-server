#pragma once

#include "HttpTypes.h"
#include "SSLSocketHandler.h"
#include "SocketHandler.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ou::http {

class Middleware {
public:
	virtual ~Middleware() = default;
	// Return true if the middleware handled the request, optionally modify request, and set response
	virtual bool process(Request &request, Response &response) = 0;
};

class RequestHandler {
public:
	virtual ~RequestHandler() = default;
	virtual Response handle(const Request &request) = 0;
};

class Server {
public:
	struct Config {
		std::filesystem::path servingDirectory;
		uint16_t port = 8080;
		int threadCount = 4;
		bool enableDirectoryIndexing = false;
#ifndef DISABLE_HTTPS
		SSLSocketHandler::Config https;
#endif
	};

	explicit Server(Config config);
	~Server();

	bool init();
	void start();
	void stop();

	void registerPathHandler(Method method, const std::string &path, const std::shared_ptr<RequestHandler> &handler);
	void registerPathHandler(Method method, const std::string &path, std::function<Response(const Request &)> handler);
	void registerPatternHandler(Method method, const std::string &pattern, const std::shared_ptr<RequestHandler> &handler);
	void registerPatternHandler(Method method, const std::string &pattern, std::function<Response(const Request &)> handler);
	void addMiddleware(std::shared_ptr<Middleware> middleware);

protected:
	std::optional<Response> handleRequest(const Request &request) const;
	std::optional<Response> handleStaticFileRequest(const Request &request) const;

private:
	void workerThread(int serverSocket);

	Config config_;
	std::atomic<bool> running_{ false };
	std::vector<std::thread> threads_;
	std::vector<int> sockets_; // One per worker thread

	std::unordered_map<Method, std::unordered_map<std::string, std::function<Response(const Request &)>>> routeHandlers_;
	std::unordered_map<Method, std::vector<std::pair<std::regex, std::function<Response(const Request &)>>>> patternHandlers_;
	std::vector<std::shared_ptr<Middleware>> middlewares_;

	std::unique_ptr<SocketHandler> socketHandler_;
};

} // namespace ou::http
