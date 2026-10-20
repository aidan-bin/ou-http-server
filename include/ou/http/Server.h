#pragma once

#include "ou/http/Error.h"
#include "ou/http/HttpTypes.h"
#include "ou/http/Router.h"
#include "ou/http/Task.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <netinet/in.h>

namespace ou::http {

class Transport;

class Server {
public:
	enum class Model : uint8_t { ThreadPerCore, EventLoop };

	struct TlsConfig {
		bool enabled = false;
		std::filesystem::path certPath;
		std::filesystem::path keyPath;
	};

	struct Config {
		uint16_t port = 8080;
		unsigned threadCount = 4;
		Model model = Model::EventLoop;

		// Requests over this limit are rejected with 413
		size_t maxRequestBytes = size_t{ 1024 } * 1024;
		// Stops a stalled connection from holding a worker thread
		std::chrono::seconds readTimeout{ 10 };

		bool keepAlive = true;

		// (Event loop only) Threads for offload() work; 0 runs it inline
		unsigned handlerThreads = 0;

		// One console line per routed request
		bool logRequests = true;

		TlsConfig https;
	};

	explicit Server(Config config, Router router = {});
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;
	Server(Server &&) = delete;
	Server &operator=(Server &&) = delete;
	~Server();

	[[nodiscard]] std::expected<void, Error> start();
	void stop();

	// The bound port; valid after start()
	[[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
	// Logs and runs the layers' postProcess on a request that never parsed
	[[nodiscard]] Response rejectRequest(int status, const sockaddr_in &clientAddr) const;

	// Routes a request for a loop, then finalizes the response; request must outlive the task
	[[nodiscard]] Task<Response> serve(Request &request, const sockaddr_in &clientAddr) const;

	// Adds keep-alive and the log line to a routed response
	[[nodiscard]] Response finalize(Response response, bool keepAlive, std::string_view logLine) const;

	// Finalizes a handler that suspended, once it returns
	[[nodiscard]] Task<Response> finalizeLater(Task<Response> handled, bool keepAlive, std::string logLine) const;

	// Maps a request path to a file under the serving root; fails with 404, or 403 (logged) outside the root
	[[nodiscard]] std::expected<std::string, int> resolveTarget(const std::string &requestPath) const;

	Config config_;
	uint16_t port_ = 0;

	Router router_;

	std::unique_ptr<Transport> transport_; // Set while started
};

} // namespace ou::http
