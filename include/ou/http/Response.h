#pragma once

#include "ou/http/Strings.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ou::http {

// HTTP field names are case-insensitive
using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;

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
	[[nodiscard]] static Response notAllowed(std::string allow);
};

} // namespace ou::http
