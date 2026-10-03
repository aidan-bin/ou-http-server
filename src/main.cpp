// Demo driver: serves ./example/www with an access log and a /kv store

#include "AccessLog.h"
#include "KVStore.h"
#include "Logging.h"
#include "Server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running{ true };

void sSignalHandler(int /*signum*/) { g_running.store(false); }

int runServer(bool useEventLoop) {
	using namespace ou::http;

	Server::Config config;
	config.servingDirectory = "./example/www";
	config.port = 8080;
	config.threadCount = 4;
	config.enableDirectoryIndexing = true;
	config.model = useEventLoop ? Server::Model::EventLoop : Server::Model::ThreadPerCore;
#ifndef DISABLE_HTTPS
	config.https = { .enabled = true, .certPath = "./example/certs/cert.pem", .keyPath = "./example/certs/key.pem" };
#endif

	Server server(config);

	const AccessLog::Config accessLogConfig = { .path = "access.log" };
	server.addMiddleware(std::make_shared<AccessLog>(accessLogConfig));

	const auto kvStore = std::make_shared<KVStore>();
	server.registerPathHandler({ Method::GET, Method::PUT, Method::DELETE }, "/kv",
														 [kvStore](const Request &request) { return kvStore->handle(request); });

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

	// TODO: arg parser?
	const bool useEventLoop = std::ranges::any_of(args.subspan(1), [](const char *arg) { return std::string_view(arg) == "--eventloop"; });

	(void)std::signal(SIGINT, sSignalHandler);
	(void)std::signal(SIGTERM, sSignalHandler);
	(void)std::signal(SIGPIPE, SIG_IGN); // A peer that disconnects mid-write would otherwise kill the process

	try {
		return runServer(useEventLoop);
	} catch (const std::exception &e) {
		LOG_ERROR("Fatal error: {}", e.what());
		return EXIT_FAILURE;
	} catch (...) {
		LOG_ERROR("Fatal error of unknown type");
		return EXIT_FAILURE;
	}
}
