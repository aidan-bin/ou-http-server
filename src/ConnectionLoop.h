#pragma once

#include "ou/http/HttpTypes.h"
#include "ou/http/Task.h"

#include <chrono>
#include <cstddef>
#include <functional>

#include <netinet/in.h>

namespace ou::http {

using Dispatch = std::function<Task<Response>(Request &request, const sockaddr_in &clientAddr)>;
using Reject = std::function<Response(int status, const sockaddr_in &clientAddr)>;

inline constexpr std::chrono::milliseconds kPollTimeout{ 100 };
inline constexpr std::chrono::milliseconds kAcceptRetryDelay{ 10 };

struct LoopConfig {
	int listenSocket = -1; // Borrowed from Transport
	size_t maxRequestBytes = 0;
	std::chrono::seconds idleTimeout{};
};

} // namespace ou::http
