#pragma once

#include <format>
#include <iostream>
#include <mutex>
#include <string_view>

namespace ou::detail {

inline std::mutex &logMutex() {
	static std::mutex mutex;
	return mutex;
}

// Worker threads would otherwise interleave partial lines
template <typename... Args> void logHelper(std::string_view format, const Args &...args) {
	const std::lock_guard<std::mutex> lock(logMutex());
	std::cout << std::vformat(format, std::make_format_args(args...)) << '\n';
}

} // namespace ou::detail

#define LOG_INFO(fmt, ...) ou::detail::logHelper("[INFO] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) ou::detail::logHelper("[WARNING] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ou::detail::logHelper("[ERROR] " fmt, ##__VA_ARGS__)
