#pragma once

#include "ou/http/Method.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <netinet/in.h>

namespace ou::http {

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

} // namespace ou::http
