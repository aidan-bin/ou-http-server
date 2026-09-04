#include "AccessLog.h"
#include "KVStore.h"
#include "Logging.h"
#include "Server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <thread>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running{ true };

void sSignalHandler(int /*signum*/) { g_running.store(false); }

int runServer() {
	using namespace ou::http;

	Server::Config config;
	config.servingDirectory = "./example/www";
	config.port = 8080;
	config.threadCount = 4;
	config.enableDirectoryIndexing = true;
#ifndef DISABLE_HTTPS
	config.https = { .enabled = true, .certPath = "./example/certs/cert.pem", .keyPath = "./example/certs/key.pem" };
#endif

	Server server(config);
	if (!server.init()) {
		LOG_ERROR("Server initialization failed");
		return EXIT_FAILURE;
	}

	const AccessLog::Config accessLogConfig = { .path = "access.log" };
	server.addMiddleware(std::make_shared<AccessLog>(accessLogConfig));

	server.registerPathHandler({ Method::GET, Method::PUT, Method::DELETE }, "/kv", std::make_shared<KVStore>());

	server.start();

	while (g_running.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	server.stop();

	LOG_INFO("Exiting");
	return EXIT_SUCCESS;
}

} // namespace

int main() {
	(void)std::signal(SIGINT, sSignalHandler);
	// A peer that disconnects mid-write would otherwise kill the process
	(void)std::signal(SIGPIPE, SIG_IGN);

	try {
		return runServer();
	} catch (const std::exception &e) {
		LOG_ERROR("Fatal error: {}", e.what());
		return EXIT_FAILURE;
	} catch (...) {
		LOG_ERROR("Fatal error of unknown type");
		return EXIT_FAILURE;
	}
}
