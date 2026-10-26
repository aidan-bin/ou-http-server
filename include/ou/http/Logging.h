#pragma once

#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace ou::http::log {

template <typename... Args>
struct Format {
	consteval Format(const char *text, std::source_location where = std::source_location::current()) : format(text), location(where) {}

	std::format_string<Args...> format;
	std::source_location location;
};

// Receives each formatted line, without the newline
using Sink = std::function<void(std::string_view line)>;

// Replaces the default sink (stdout); an empty sink restores it
void setSink(Sink sink);

namespace detail {

	enum class Level : uint8_t { Info, Warn, Error };

	void emit(Level level, const std::string &line) noexcept;

	[[nodiscard]] constexpr std::string_view fileName(std::string_view path) { return path.substr(path.find_last_of('/') + 1); }

	template <typename... Args>
	void writeLine(Level level, const Format<const Args &...> &format, const Args &...args) noexcept {
		try {
			const std::string message = std::format(format.format, args...);
			emit(level, level == Level::Info ? std::format("[INFO] {}", message)
			                                 : std::format("[{}] {}:{}: {}", level == Level::Warn ? "WARNING" : "ERROR",
			                                               fileName(format.location.file_name()), format.location.line(), message));
		} catch (...) {
			(void)std::fputs("[ERROR] Failed to write a log line\n", stderr);
		}
	}

} // namespace detail

template <typename... Args>
void info(Format<std::type_identity_t<const Args &>...> format, const Args &...args) noexcept {
	detail::writeLine(detail::Level::Info, format, args...);
}

template <typename... Args>
void warn(Format<std::type_identity_t<const Args &>...> format, const Args &...args) noexcept {
	detail::writeLine(detail::Level::Warn, format, args...);
}

template <typename... Args>
void error(Format<std::type_identity_t<const Args &>...> format, const Args &...args) noexcept {
	detail::writeLine(detail::Level::Error, format, args...);
}

} // namespace ou::http::log
