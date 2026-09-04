#pragma once

#include "HttpTypes.h"
#include "Server.h"

#include <filesystem>
#include <mutex>

#include <netinet/in.h>

namespace ou::http {

class AccessLog : public Middleware {
public:
	struct Config {
		std::filesystem::path path;
	};

	explicit AccessLog(Config config);

	void log(const Request &request, const Response &response, const sockaddr_in &clientAddr);

	void postProcess(const Request &request, const Response &response) override final;

private:
	Config config_;
	std::mutex mutex_;
};

} // namespace ou::http
