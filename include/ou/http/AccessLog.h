#pragma once

#include "ou/http/HttpTypes.h"
#include "ou/http/Middleware.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>

namespace ou::http {

// Batches entries and appends them by path on each flush, so a log rotated or removed externally is
// simply recreated
class AccessLog final : public Middleware {
public:
	struct Config {
		std::filesystem::path path;
		size_t flushBytes = size_t{ 64 } * 1024;
		std::chrono::milliseconds flushInterval{ 1000 };
	};

	explicit AccessLog(Config config);
	AccessLog(const AccessLog &) = delete;
	AccessLog &operator=(const AccessLog &) = delete;
	AccessLog(AccessLog &&) = delete;
	AccessLog &operator=(AccessLog &&) = delete;
	~AccessLog() override;

	void flush();

	void after(const Request &request, Response &response) final;

private:
	void flushLocked();

	Config config_;
	std::mutex mutex_;
	std::string buffer_;
	std::chrono::steady_clock::time_point lastFlush_{ std::chrono::steady_clock::now() };
};

} // namespace ou::http
