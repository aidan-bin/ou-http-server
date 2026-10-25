#pragma once

#include "ou/http/Error.h"
#include "ou/http/Router.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>

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
		// Closes a connection idle for this long
		std::chrono::seconds idleTimeout{ 10 };

		bool keepAlive = true;

		// (Event loop only) Threads for offload() work; 0 runs it inline
		unsigned handlerThreads = 0;

		// One console line per routed request
		bool logRequests = true;

		TlsConfig https;
	};

	// Fails on a config that could never serve or a port that cannot be opened; takes ownership of the router
	[[nodiscard]] static std::expected<Server, Error> start(const Config &config, Router router = {});

	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;
	Server(Server &&) noexcept;
	Server &operator=(Server &&) = delete;

	// Stops the handler pool first, then the loops, then closes the listening socket
	~Server();

	[[nodiscard]] uint16_t port() const noexcept;

private:
	explicit Server(std::unique_ptr<Transport> transport);

	std::unique_ptr<Transport> transport_;
};

} // namespace ou::http
