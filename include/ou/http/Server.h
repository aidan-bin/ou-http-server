#pragma once

#include "ou/http/HttpTypes.h"
#include "ou/http/Router.h"
#include "ou/http/Task.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

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
		std::filesystem::path servingDirectory;
		uint16_t port = 8080;
		unsigned threadCount = 4;
		bool enableDirectoryIndexing = false;
		Model model = Model::EventLoop;

		// Requests over this limit are rejected with 413
		size_t maxRequestBytes = size_t{ 1024 } * 1024;
		// Stops a stalled connection from holding a worker thread
		std::chrono::seconds readTimeout{ 10 };

		bool keepAlive = true;

		// (Event loop only) Threads for offload() work; 0 runs it inline
		unsigned handlerThreads = 0;

		// Loads files into memory at construction (up to this many bytes total). 0 disables.
		// Files that don't fit are served from disk. Changes on disk afterwards are not seen.
		size_t preloadBytes = 0;

		// Skips resolving symlinks on each disk request. ".." is still rejected, but a symlink inside the
		// serving directory can point anywhere.
		bool skipSymlinkCheck = false;

		// One console line per routed request
		bool logRequests = true;

		TlsConfig https;
	};

	struct StartError {
		std::string step;
		std::error_code code; // Empty when the step has no errno; its reason is then part of step

		[[nodiscard]] std::string message() const {
			return code ? std::format("Failed to {}: {}", step, code.message()) : std::format("Failed to {}", step);
		}
	};

	explicit Server(Config config, Router router = {});
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;
	Server(Server &&) = delete;
	Server &operator=(Server &&) = delete;
	~Server();

	[[nodiscard]] std::expected<void, StartError> start();
	void stop();

	// The bound port; valid after start()
	[[nodiscard]] uint16_t port() const noexcept { return port_; }

protected:
	[[nodiscard]] Response handleStaticFileRequest(const Request &request) const;

private:
	struct PreloadedFile {
		std::string body;
		std::string_view contentType;
	};

	void preload();

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
	std::filesystem::path servingRoot_;
	std::unordered_map<std::string, PreloadedFile> preloaded_;
	uint16_t port_ = 0;

	Router router_;

	std::unique_ptr<Transport> transport_; // Set while started
};

} // namespace ou::http
