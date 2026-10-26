#pragma once

#include "UniqueFd.h"
#include "ou/http/Error.h"

#include <cstdint>
#include <expected>
#include <system_error>

#include <netinet/in.h>

namespace ou::http {

struct Listener {
	UniqueFd socket;
	uint16_t port = 0;
};

// A non-blocking TCP listener on every interface, so an accept loop can poll for shutdown
[[nodiscard]] std::expected<Listener, Error> openListener(uint16_t port);

// Empty if nothing is queued or the error is transient
[[nodiscard]] std::expected<UniqueFd, std::error_code> acceptClient(int listenSocket, sockaddr_in &clientAddr);

} // namespace ou::http
