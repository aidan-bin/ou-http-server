#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace ou::http {

enum class Method : uint8_t { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, CONNECT, TRACE };

inline constexpr auto kMethodNames = std::to_array<std::pair<std::string_view, Method>>({
		{ "GET", Method::GET },
		{ "POST", Method::POST },
		{ "PUT", Method::PUT },
		{ "DELETE", Method::DELETE },
		{ "PATCH", Method::PATCH },
		{ "HEAD", Method::HEAD },
		{ "OPTIONS", Method::OPTIONS },
		{ "CONNECT", Method::CONNECT },
		{ "TRACE", Method::TRACE },
});

[[nodiscard]] constexpr std::optional<Method> stringToMethod(std::string_view method) {
	for (const auto &[name, value] : kMethodNames) {
		if (name == method)
			return value;
	}
	return std::nullopt;
}

[[nodiscard]] constexpr std::string_view methodToString(Method method) {
	for (const auto &[name, value] : kMethodNames) {
		if (value == method)
			return name;
	}
	return "UNKNOWN";
}

} // namespace ou::http

template <>
struct std::formatter<ou::http::Method> : std::formatter<std::string_view> {
	auto format(const ou::http::Method &method, auto &ctx) const {
		return std::formatter<std::string_view>::format(ou::http::methodToString(method), ctx);
	}
};
