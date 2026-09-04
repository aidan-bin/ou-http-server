#pragma once

#include "HttpTypes.h"
#include "Server.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string_view>

namespace ou::detail {

inline std::mutex &logMutex() {
	static std::mutex mutex;
	return mutex;
}

} // namespace ou::detail

#define LOG_INFO(fmt, ...) log_helper("[INFO] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_helper("[WARNING] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_helper("[ERROR] " fmt, ##__VA_ARGS__)

template <typename... Args> void log_helper(std::string_view format, const Args &...args) {
	const std::lock_guard<std::mutex> lock(ou::detail::logMutex());
	std::cout << std::vformat(format, std::make_format_args(args...)) << '\n';
}

namespace ou::http {

class AccessLog : public Middleware {
public:
	struct Config {
		std::filesystem::path path;
	};

	explicit AccessLog(Config config);

	void log(const Request &request, const Response &response, const sockaddr_in &clientAddr);

	bool process(Request &request, Response &response) override;
	void postProcess(const Request &request, const Response &response) override;

private:
	Config config_;
	std::mutex mutex_;
};

} // namespace ou::http
