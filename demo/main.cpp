// Demo driver

#include "KVStore.h"
#include "ou/http/AccessLog.h"
#include "ou/http/Awaitables.h"
#include "ou/http/Logging.h"
#include "ou/http/Server.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running{ true };

void sSignalHandler(int /*signum*/) { g_running.store(false); }

// Short-circuits writes before routing, e.g., PUT /kv
class ReadOnly final : public ou::http::Middleware {
public:
	std::optional<ou::http::Response> process(ou::http::Request &request) final {
		if (request.method == ou::http::Method::GET || request.method == ou::http::Method::HEAD)
			return std::nullopt;
		return ou::http::Response::plainText(405, "Server is read-only");
	}
};

std::chrono::milliseconds sDemoDuration(int ms) { return std::chrono::milliseconds(std::clamp(ms, 0, 10'000)); }

int runServer(const auto &hasFlag, const auto &numberFlag) {
	using namespace ou::http;

	Server::Config config;
	config.servingDirectory = "./example/www";
	config.port = 8080;
	config.threadCount = 4;
	config.enableDirectoryIndexing = true;
	config.model = hasFlag("--thread-per-core") ? Server::Model::ThreadPerCore : Server::Model::EventLoop;
	config.preloadBytes = hasFlag("--no-preload") ? 0 : size_t{ 64 } * 1024 * 1024;
	config.skipSymlinkCheck = hasFlag("--skip-symlink-check");
	config.logRequests = !hasFlag("--no-request-log");
	config.handlerThreads = numberFlag("--handler-threads=");
	config.https = { .enabled = !hasFlag("--no-tls"), .certPath = "./example/certs/cert.pem", .keyPath = "./example/certs/key.pem" };

	Server server(config);

	if (!hasFlag("--no-access-log")) {
		const AccessLog::Config accessLogConfig = { .path = "access.log" };
		server.addMiddleware(std::make_shared<AccessLog>(accessLogConfig));
	}

	if (hasFlag("--read-only"))
		server.addMiddleware(std::make_shared<ReadOnly>());

	const auto kvStore = std::make_shared<KVStore>();
	server.registerPathHandler({ Method::GET, Method::PUT, Method::DELETE }, "/kv",
														 [kvStore](const Request &request, Query<"key"> key) { return kvStore->handle(request, key.value); });
	server.registerPatternHandler({ Method::GET, Method::PUT, Method::DELETE }, "/kv/{key}",
																[kvStore](const Request &request, Path<"key"> key) { return kvStore->handle(request, key.value); });

	server.registerPathHandler(Method::GET, "/whoami",
														 [](Header<"User-Agent"> agent) { return Response::plainText(200, std::move(agent.value)); });

	server.registerPathHandler(Method::GET, "/sleep", [](Query<"ms", int> ms) -> Task<Response> {
		co_await ou::http::sleepFor(sDemoDuration(ms.value));
		co_return Response::plainText(200, "OK");
	});

	server.registerPathHandler(Method::GET, "/work", [](Query<"ms", int> ms) -> Task<Response> {
		co_return co_await ou::http::offload([deadline = std::chrono::steady_clock::now() + sDemoDuration(ms.value)] {
			while (std::chrono::steady_clock::now() < deadline) {
			}
			return Response::plainText(200, "OK");
		});
	});

	if (const auto started = server.start(); !started) {
		ou::logError("Server failed to start: {}", started.error().message());
		return EXIT_FAILURE;
	}

	using namespace std::chrono_literals;
	while (g_running.load()) {
		std::this_thread::sleep_for(100ms);
	}

	server.stop();

	ou::logInfo("Exiting");
	return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
	const std::span args(argv, static_cast<size_t>(argc));

	const auto hasFlag = [&](std::string_view flag) {
		return std::ranges::contains(args.subspan(1), flag);
	};

	const auto numberFlag = [&](std::string_view prefix) {
		unsigned value = 0;
		for (const std::string_view arg : args.subspan(1)) {
			if (arg.starts_with(prefix))
				(void)std::from_chars(std::to_address(arg.begin() + prefix.size()), std::to_address(arg.end()), value);
		}
		return value;
	};

	(void)std::signal(SIGINT, sSignalHandler);
	(void)std::signal(SIGTERM, sSignalHandler);
	(void)std::signal(SIGPIPE, SIG_IGN); // A peer that disconnects mid-write would otherwise kill the process

	try {
		return runServer(hasFlag, numberFlag);
	} catch (const std::exception &e) {
		ou::logError("Fatal error: {}", e.what());
		return EXIT_FAILURE;
	} catch (...) {
		ou::logError("Fatal error of unknown type");
		return EXIT_FAILURE;
	}
}
