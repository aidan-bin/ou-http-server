#pragma once

#include "HttpTypes.h"
#include "Task.h"

#include <chrono>
#include <cstddef>
#include <functional>

#include <netinet/in.h>

namespace ou::http {

using Dispatch = std::function<Task<Response>(Request &request, const sockaddr_in &clientAddr)>;
using Reject = std::function<Response(int status, const sockaddr_in &clientAddr)>;

struct LoopConfig {
	int listenSocket = -1; // Borrowed from Server
	size_t maxRequestBytes = size_t{ 1024 } * 1024;
	std::chrono::seconds idleTimeout{ 10 };
};

} // namespace ou::http
