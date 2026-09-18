#pragma once

#include "HttpTypes.h"
#include "Server.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>

#include <netinet/in.h>

namespace ou::http {

class AccessLog final : public Middleware {
public:
	struct Config {
		std::filesystem::path path;
		size_t flushBytes = 64 * 1024;
		std::chrono::milliseconds flushInterval{ 1000 };
	};

	explicit AccessLog(Config config);
	~AccessLog() override;

	void log(const Request &request, const Response &response, const sockaddr_in &clientAddr);

	void flush();

	void postProcess(const Request &request, const Response &response) override final;

private:
	void flushLocked();

	Config config_;
	std::mutex mutex_;
	std::string pending_;
	std::chrono::steady_clock::time_point lastFlush_{ std::chrono::steady_clock::now() };
};

} // namespace ou::http
