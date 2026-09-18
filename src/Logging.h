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

template <typename... Args> void logHelper(bool flush, std::string_view format, const Args &...args) {
	const std::lock_guard<std::mutex> lock(logMutex());

	std::cout << std::vformat(format, std::make_format_args(args...)) << '\n';
	if (flush)
		std::cout.flush();
}

} // namespace ou::detail

#define LOG_INFO(fmt, ...) ou::detail::logHelper(false, "[INFO] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) ou::detail::logHelper(true, "[WARNING] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ou::detail::logHelper(true, "[ERROR] " fmt, ##__VA_ARGS__)
