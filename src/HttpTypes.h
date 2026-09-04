#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <netinet/in.h>

namespace ou::http {

enum class Method : uint8_t { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, CONNECT, TRACE };

inline constexpr std::array<std::pair<std::string_view, Method>, 9> kMethodNames{ {
		{ "GET", Method::GET },
		{ "POST", Method::POST },
		{ "PUT", Method::PUT },
		{ "DELETE", Method::DELETE },
		{ "PATCH", Method::PATCH },
		{ "HEAD", Method::HEAD },
		{ "OPTIONS", Method::OPTIONS },
		{ "CONNECT", Method::CONNECT },
		{ "TRACE", Method::TRACE },
} };

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

std::ostream &operator<<(std::ostream &os, Method method);

struct CaseInsensitiveLess {
	using is_transparent = void;
	[[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
};

// HTTP field names are case-insensitive
using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;

[[nodiscard]] bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept;

// Decodes %XX escapes and '+' as space; invalid escapes are left verbatim
[[nodiscard]] std::string percentDecode(std::string_view input);

// Splits "a=1&b=2" and percent-decodes each name and value; later duplicates win
[[nodiscard]] std::map<std::string, std::string> parseQuery(std::string_view query);

[[nodiscard]] constexpr std::string_view defaultReasonPhrase(int statusCode) {
	switch (statusCode) {
	case 200:
		return "OK";
	case 201:
		return "Created";
	case 204:
		return "No Content";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 408:
		return "Request Timeout";
	case 413:
		return "Content Too Large";
	case 500:
		return "Internal Server Error";
	case 505:
		return "HTTP Version Not Supported";
	default:
		return "Unknown";
	}
}

struct Request {
	Method method{ Method::GET };
	std::string path;	 // Decoded
	std::string query; // Raw, must be percent-decoded
	Headers headers;
	std::string body;

	std::optional<sockaddr_in> clientAddr;

	[[nodiscard]] static std::optional<Request> parse(std::string_view raw);
	[[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
	[[nodiscard]] std::optional<size_t> contentLength() const;

private:
	static std::pair<std::string_view, std::string_view> split_at(std::string_view str, char delimiter);
	static std::string_view trim(std::string_view str);
};

struct Response {
	int statusCode = 200;
	std::string reasonPhrase; // Derived from statusCode if empty
	Headers headers;
	std::string body;

	[[nodiscard]] std::string serialize() const;

	[[nodiscard]] static Response plainText(int statusCode, std::string body);
	[[nodiscard]] static Response error(int statusCode);
};

} // namespace ou::http

template <> struct std::formatter<ou::http::Method> : std::formatter<std::string_view> {
	auto format(const ou::http::Method &method, auto &ctx) const {
		return std::formatter<std::string_view>::format(ou::http::methodToString(method), ctx);
	}
};
