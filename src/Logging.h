#pragma once

#include "HttpTypes.h"

#include <cstdio>
#include <optional>
#include <filesystem>
#include <netinet/in.h>

#define LOG_INFO(fmt, ...)  printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) printf("[WARNING] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) printf("[ERROR] " fmt "\n", ##__VA_ARGS__)

namespace ou::http {

class AccessLog {
public:
	struct Config {
		bool enabled = false;
		std::filesystem::path path;
		size_t maxSizeBytes;
	};

    explicit AccessLog(const Config& config);
	~AccessLog() = default;

    void log(const Request& request, const Response& response, const sockaddr_in& clientAddr);

private:
    void enforceSizeLimit();

    Config config_;
};

} // namespace ou::http
