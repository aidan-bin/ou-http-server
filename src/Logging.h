#pragma once

#include <format>
#include <iostream>
#include <mutex>

namespace ou::detail {

inline std::mutex &logMutex() {
	static std::mutex mutex;
	return mutex;
}

// Locked so lines from concurrent workers don't interleave
template <typename... Args> void writeLine(bool flush, std::format_string<const Args &...> format, const Args &...args) {
	const std::lock_guard<std::mutex> lock(logMutex());

	std::cout << std::format(format, args...) << '\n';
	if (flush)
		std::cout.flush();
}

} // namespace ou::detail

#define LOG_INFO(fmt, ...) ou::detail::writeLine(false, "[INFO] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) ou::detail::writeLine(true, "[WARNING] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ou::detail::writeLine(true, "[ERROR] " fmt, ##__VA_ARGS__)
