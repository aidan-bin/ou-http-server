#pragma once

#include <cstdio>
#include <format>
#include <iostream>
#include <mutex>
#include <string>

namespace ou::detail {

inline std::mutex &logMutex() {
	static std::mutex mutex;
	return mutex;
}

template <typename... Args> void writeLine(bool flush, std::format_string<const Args &...> format, const Args &...args) noexcept {
	try {
		const std::string line = std::format(format, args...);
		const std::lock_guard<std::mutex> lock(logMutex());

		std::cout << line << '\n';
		if (flush)
			std::cout.flush();
	} catch (...) {
		(void)std::fputs("[ERROR] Failed to write a log line\n", stderr);
	}
}

} // namespace ou::detail

#define LOG_INFO(fmt, ...) ou::detail::writeLine(false, "[INFO] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) ou::detail::writeLine(true, "[WARNING] " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ou::detail::writeLine(true, "[ERROR] " fmt, ##__VA_ARGS__)
