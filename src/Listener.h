#pragma once

#include "UniqueFd.h"

#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>

namespace ou {

struct SocketError {
	std::string_view step;
	std::error_code code;
};

struct Listener {
	UniqueFd socket;
	uint16_t port = 0;
};

// A non-blocking TCP listener on every interface, so an accept loop can poll for shutdown
[[nodiscard]] std::expected<Listener, SocketError> openListener(uint16_t port);

} // namespace ou
