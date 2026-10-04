// Demo driver: serves ./example/www with an access log, a /kv store, and a /sleep?ms= handler

#include "AccessLog.h"
#include "KVStore.h"
#include "Logging.h"
#include "Server.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running{ true };

void sSignalHandler(int /*signum*/) { g_running.store(false); }

int runServer(const auto &hasFlag) {
	using namespace ou::http;

	Server::Config config;
	config.servingDirectory = "./example/www";
	config.port = 8080;
	config.threadCount = 4;
	config.enableDirectoryIndexing = true;
	config.model = hasFlag("--eventloop") ? Server::Model::EventLoop : Server::Model::ThreadPerCore;
	config.preloadBytes = hasFlag("--no-preload") ? 0 : size_t{ 64 } * 1024 * 1024;
	config.skipSymlinkCheck = hasFlag("--skip-symlink-check");
	config.logRequests = !hasFlag("--no-request-log");
#ifndef DISABLE_HTTPS
	config.https = { .enabled = true, .certPath = "./example/certs/cert.pem", .keyPath = "./example/certs/key.pem" };
#endif

	Server server(config);

	if (!hasFlag("--no-access-log")) {
		const AccessLog::Config accessLogConfig = { .path = "access.log" };
		server.addMiddleware(std::make_shared<AccessLog>(accessLogConfig));
	}

	const auto kvStore = std::make_shared<KVStore>();
	server.registerPathHandler({ Method::GET, Method::PUT, Method::DELETE }, "/kv",
														 [kvStore](const Request &request) { return kvStore->handle(request); });

	server.registerPathHandler(Method::GET, "/sleep", [](const Request &request) {
		const auto params = parseQuery(request.query);
		const auto it = params.find("ms");
		int ms = 100;
		if (it != params.end()) {
			const char *end = std::to_address(it->second.end());
			const auto [stopped, ec] = std::from_chars(it->second.data(), end, ms);
			if (ec != std::errc{} || stopped != end)
				return Response::plainText(400, "Bad ms parameter");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(ms, 0, 10'000)));
		return Response::plainText(200, "OK");
	});

	if (!server.start()) {
		LOG_ERROR("Server failed to start");
		return EXIT_FAILURE;
	}

	while (g_running.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	server.stop();

	LOG_INFO("Exiting");
	return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
	const std::span args(argv, static_cast<size_t>(argc));

	const auto hasFlag = [&](std::string_view flag) {
		return std::ranges::any_of(args.subspan(1), [&](const char *arg) { return arg == flag; });
	};

	(void)std::signal(SIGINT, sSignalHandler);
	(void)std::signal(SIGTERM, sSignalHandler);
	(void)std::signal(SIGPIPE, SIG_IGN); // A peer that disconnects mid-write would otherwise kill the process

	try {
		return runServer(hasFlag);
	} catch (const std::exception &e) {
		LOG_ERROR("Fatal error: {}", e.what());
		return EXIT_FAILURE;
	} catch (...) {
		LOG_ERROR("Fatal error of unknown type");
		return EXIT_FAILURE;
	}
}
