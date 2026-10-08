// Demo driver: serves ./example/www with an access log, a /kv store, and /sleep?ms= and /work?ms= handlers

#include "AccessLog.h"
#include "Awaitables.h"
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
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running{ true };

void sSignalHandler(int /*signum*/) { g_running.store(false); }

std::optional<std::chrono::milliseconds> sMillisParam(const ou::http::Request &request) {
	const auto params = ou::http::parseQuery(request.query);
	const auto it = params.find("ms");
	if (it == params.end())
		return std::nullopt;

	int ms = 0;
	const char *end = std::to_address(it->second.end());
	const auto [stopped, ec] = std::from_chars(it->second.data(), end, ms);
	if (ec != std::errc{} || stopped != end)
		return std::nullopt;
	return std::chrono::milliseconds(std::clamp(ms, 0, 10'000));
}

int runServer(const auto &hasFlag, const auto &numberFlag) {
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
	config.handlerThreads = numberFlag("--handler-threads=");
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

	server.registerPathHandler(Method::GET, "/sleep", [](const Request &request) -> ou::Task<Response> {
		const std::optional<std::chrono::milliseconds> duration = sMillisParam(request);
		if (!duration)
			co_return Response::plainText(400, "Missing or bad ms parameter");

		co_await ou::http::sleepFor(*duration);
		co_return Response::plainText(200, "OK");
	});

	server.registerPathHandler(Method::GET, "/work", [](const Request &request) -> ou::Task<Response> {
		const std::optional<std::chrono::milliseconds> duration = sMillisParam(request);
		if (!duration)
			co_return Response::plainText(400, "Missing or bad ms parameter");

		co_return co_await ou::http::offload([deadline = std::chrono::steady_clock::now() + *duration] {
			while (std::chrono::steady_clock::now() < deadline) {
			}
			return Response::plainText(200, "OK");
		});
	});

	if (!server.start()) {
		ou::logError("Server failed to start");
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
