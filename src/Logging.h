#pragma once

#include <cstdint>
#include <cstdio>
#include <format>
#include <iostream>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace ou {

template <typename... Args>
struct LogFormat {
	consteval LogFormat(const char *text, std::source_location where = std::source_location::current()) // NOLINT(google-explicit-constructor)
			: format(text), location(where) {}

	std::format_string<Args...> format;
	std::source_location location;
};

namespace detail {

	enum class LogLevel : uint8_t { Info, Warn, Error };

	inline std::mutex &logMutex() {
		static std::mutex mutex;
		return mutex;
	}

	[[nodiscard]] constexpr std::string_view fileName(std::string_view path) { return path.substr(path.find_last_of('/') + 1); }

	template <typename... Args>
	void writeLine(LogLevel level, const LogFormat<const Args &...> &format, const Args &...args) noexcept {
		try {
			const std::string message = std::format(format.format, args...);
			const std::string line = level == LogLevel::Info
																	 ? std::format("[INFO] {}\n", message)
																	 : std::format("[{}] {}:{}: {}\n", level == LogLevel::Warn ? "WARNING" : "ERROR",
																								 fileName(format.location.file_name()), format.location.line(), message);

			const std::lock_guard<std::mutex> lock(logMutex());
			std::cout << line;
			if (level != LogLevel::Info)
				std::cout.flush();
		} catch (...) {
			(void)std::fputs("[ERROR] Failed to write a log line\n", stderr);
		}
	}

} // namespace detail

template <typename... Args>
void logInfo(LogFormat<std::type_identity_t<const Args &>...> format, const Args &...args) noexcept {
	detail::writeLine(detail::LogLevel::Info, format, args...);
}

template <typename... Args>
void logWarn(LogFormat<std::type_identity_t<const Args &>...> format, const Args &...args) noexcept {
	detail::writeLine(detail::LogLevel::Warn, format, args...);
}

template <typename... Args>
void logError(LogFormat<std::type_identity_t<const Args &>...> format, const Args &...args) noexcept {
	detail::writeLine(detail::LogLevel::Error, format, args...);
}

} // namespace ou
