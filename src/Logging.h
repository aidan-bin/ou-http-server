#pragma once

#include "HttpTypes.h"
#include "Server.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <optional>

#define LOG_INFO(fmt, ...) log_helper("[INFO] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_helper("[WARNING] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_helper("[ERROR] " fmt, ##__VA_ARGS__)

template <typename... Args> void log_helper(std::string_view format, Args &&...args) {
	std::cout << std::vformat(format, std::make_format_args(args...)) << '\n';
}

namespace ou::http {

class Middleware;

class AccessLog : public Middleware {
public:
	struct Config {
		std::filesystem::path path;
		size_t maxSizeBytes;
	};

	explicit AccessLog(Config config);
	~AccessLog() override = default;

	void log(const Request &request, const Response &response, const sockaddr_in &clientAddr);

	bool process(Request &request, Response &response) override;

private:
	void enforceSizeLimit() const;

	Config config_;
};

} // namespace ou::http
