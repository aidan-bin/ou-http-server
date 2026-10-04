#pragma once

#include "ConnectionLoop.h"
#include "HttpTypes.h"
#include "SSLSocketHandler.h"
#include "SocketHandler.h"
#include "UniqueFd.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
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

	// Runs before routing. Return true to answer with `response` and skip routing; may modify `request`
	virtual bool process(Request & /*request*/, Response & /*response*/) { return false; }

	// Runs after every request (including short-circuited and rejected ones)
	virtual void postProcess(const Request & /*request*/, const Response & /*response*/) {}
};

class Server {
public:
	using Handler = std::function<Response(const Request &)>;

	enum class Model : uint8_t { ThreadPerCore, EventLoop };

	struct Config {
		std::filesystem::path servingDirectory;
		uint16_t port = 8080;
		unsigned threadCount = 4;
		bool enableDirectoryIndexing = false;
		Model model = Model::ThreadPerCore;

		// Requests over this limit are rejected with 413
		size_t maxRequestBytes = size_t{ 1024 } * 1024;
		// Stops a stalled connection from holding a worker thread
		std::chrono::seconds readTimeout{ 10 };

		bool keepAlive = true;

		// Loads files into memory at construction (up to this many bytes total). 0 disables.
		// Files that don't fit are served from disk. Changes on disk afterwards are not seen.
		size_t preloadBytes = 0;

		// Skips resolving symlinks on each disk request. ".." is still rejected, but a symlink inside the
		// serving directory can point anywhere.
		bool skipSymlinkCheck = false;

		// One console line per routed request
		bool logRequests = true;

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

	[[nodiscard]] bool start();
	void stop();

	// The bound port; valid after start()
	[[nodiscard]] uint16_t port() const noexcept { return port_; }

	void addMiddleware(std::shared_ptr<Middleware> middleware);

	// Re-registering an exact path replaces its handler; patterns accumulate and the first match wins
	void registerPathHandler(Method method, const std::string &path, Handler handler);
	void registerPatternHandler(Method method, const std::string &pattern, Handler handler);
	void registerPathHandler(std::initializer_list<Method> methods, const std::string &path, const Handler &handler);
	void registerPatternHandler(std::initializer_list<Method> methods, const std::string &pattern, const Handler &handler);

protected:
	// Taken by value: middlewares mutate the request, and later stages read the mutated copy
	[[nodiscard]] Response handleRequest(Request request) const;
	[[nodiscard]] Response handleStaticFileRequest(const Request &request) const;

	// Logs and runs middleware postProcess on a request that never parsed
	[[nodiscard]] Response rejectRequest(int status, const sockaddr_in &clientAddr) const;

private:
	struct PatternRoute {
		Method method;
		std::regex pattern;
		Handler handler;
	};

	struct PreloadedFile {
		std::string body;
		std::string_view contentType;
	};

	void preload();
	void runLoop();

	// Wraps handleRequest with what needs the connection (client address, keep-alive, and the log line)
	[[nodiscard]] Response serve(Request request, const sockaddr_in &clientAddr) const;

	Config config_;
	std::filesystem::path servingRoot_;
	std::unordered_map<std::string, PreloadedFile> preloaded_;
	uint16_t port_ = 0;
	std::atomic<bool> running_{ false };
	std::vector<std::thread> threads_;
	UniqueFd listenSocket_; // Shared by every worker thread

	// Exact paths are looked up; patterns are tried in registration order
	std::map<std::pair<Method, std::string>, Handler> pathHandlers_;
	std::vector<PatternRoute> patternHandlers_;
	std::vector<std::shared_ptr<Middleware>> middlewares_;

	std::unique_ptr<SocketHandler> socketHandler_;
};

} // namespace ou::http
