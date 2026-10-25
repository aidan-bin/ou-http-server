#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <netinet/in.h>

namespace ou::http {

// --- Methods ---

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

std::ostream &operator<<(std::ostream &os, Method method);

// --- Strings ---

[[nodiscard]] bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept;

[[nodiscard]] std::string_view trim(std::string_view str);

struct CaseInsensitiveLess {
	using is_transparent = void;
	[[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
};

// HTTP field names are case-insensitive
using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;

// Parses all of text as a number; nullopt if anything is left over
template <std::integral T>
[[nodiscard]] std::optional<T> parseNumber(std::string_view text, int base = 10) {
	T value{};
	const char *end = std::to_address(text.end());
	const auto [stopped, ec] = std::from_chars(std::to_address(text.begin()), end, value, base);
	if (ec != std::errc{} || stopped != end)
		return std::nullopt;
	return value;
}

// --- URLs ---

// Decodes %XX escapes; invalid escapes are left verbatim. '+' means space only in query/form data
[[nodiscard]] std::string percentDecode(std::string_view input, bool plusAsSpace = true);

// Escapes everything outside the unreserved set, leaving '/' intact so a path stays a path
[[nodiscard]] std::string percentEncode(std::string_view input);

// Splits "a=1&b=2" and percent-decodes each name and value; later duplicates win
[[nodiscard]] std::map<std::string, std::string> parseQuery(std::string_view query);

// --- Status codes ---

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
	case 501:
		return "Not Implemented";
	case 505:
		return "HTTP Version Not Supported";
	default:
		return "Unknown";
	}
}

// --- Sockets ---

// inet_ntoa returns a shared static buffer, so worker threads need this instead
[[nodiscard]] std::string addressToString(const sockaddr_in &address);

// --- Messages ---

struct Request {
	Method method{ Method::GET };
	bool http10 = false;
	std::string path;       // Decoded
	std::string_view query; // Raw, must be percent-decoded
	std::vector<std::pair<std::string_view, std::string_view>> headers;
	std::string_view body;

	std::optional<sockaddr_in> clientAddr;

	// Pattern route capture groups as (offset, length) into path.
	// Offsets, not string_views: with SSO, views would not survive a Request move.
	std::vector<std::pair<size_t, size_t>> pathParams;
	// Names of groups 1.. of a {name} route; views the route, which outlives the request
	std::span<const std::string> pathParamNames;

	// Parses the head without the blank line; fails with 505 for an unsupported version, else 400
	[[nodiscard]] static std::expected<Request, int> parse(std::string_view head);

	// Case-insensitive; the last duplicate wins
	[[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
	[[nodiscard]] std::optional<size_t> contentLength() const;

	// Capture group index (0 = full match); nullopt if out of range
	[[nodiscard]] std::optional<std::string_view> pathParam(size_t index) const;
	// nullopt if the route has no such {name}
	[[nodiscard]] std::optional<std::string_view> pathParam(std::string_view name) const;

	// HTTP/1.1 keeps the connection open unless asked not to
	[[nodiscard]] bool wantsKeepAlive() const;
};

struct Response {
	int statusCode = 200;
	std::string reasonPhrase; // Derived from statusCode if empty
	Headers headers;
	std::string body;
	bool keepAlive = false;              // Emitted as the Connection header
	std::optional<size_t> contentLength; // Overrides body.size() in the header; a HEAD response keeps the GET length

	[[nodiscard]] std::string serialize() const;

	[[nodiscard]] static Response plainText(int statusCode, std::string body);
	[[nodiscard]] static Response error(int statusCode);
};

} // namespace ou::http

template <>
struct std::formatter<ou::http::Method> : std::formatter<std::string_view> {
	auto format(const ou::http::Method &method, auto &ctx) const {
		return std::formatter<std::string_view>::format(ou::http::methodToString(method), ctx);
	}
};
