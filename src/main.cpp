#include "Logging.h"
#include "Server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

std::atomic<bool> g_running{ true };

void signalHandler(int signum) {
	LOG_INFO("Interrupt signal ({}) received", signum);
	g_running.store(false);
}

int main(int argc, char *argv[]) {
	std::signal(SIGINT, signalHandler);

	ou::http::Server::Config config;
	config.servingDirectory = "./example/www";
	config.port = 8080;
	config.threadCount = 4;
	config.enableDirectoryIndexing = true;
	config.accessLog = { .enabled = true, .path = "access.log", .maxSizeBytes = 10 * 1024 * 1024 };
#ifndef DISABLE_HTTPS
	config.https = { .enabled = true, .certPath = "./example/certs/cert.pem", .keyPath = "./example/certs/key.pem" };
#endif

	ou::http::Server server(config);
	if (!server.init()) {
		LOG_ERROR("Server initialization failed");
		return EXIT_FAILURE;
	}

	std::thread serverThread([&server]() { server.start(); });

	while (g_running.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	server.stop();
	if (serverThread.joinable()) {
		serverThread.join();
	}

	LOG_INFO("Exiting");
	return EXIT_SUCCESS;
}
