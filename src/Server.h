#pragma once

#include "HttpTypes.h"

#include <string>
#include <vector>
#include <thread>
#include <optional>
#include <filesystem>
#include <memory>
#include <netinet/in.h>

namespace ou::http {

class Server {
public:
	struct Config {
		std::filesystem::path servingDirectory;
		uint16_t port = 8080;
		int threadCount = 4;
		bool enableDirectoryIndexing = false;
	};

	explicit Server(Config config);
	~Server();

	bool init();
	void start();
	void stop();

private:
	void workerThread(int serverSocket);
	std::optional<Response> handleRequest(const Request& request);

	Config config_;
	std::atomic<bool> running_{false};
	std::vector<std::thread> threads_;
	std::vector<int> sockets_; // One per worker thread
};

} // namespace ou::http
