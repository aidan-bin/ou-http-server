#pragma once

#include "Dispatcher.h"
#include "SocketIo.h"
#include "ThreadPool.h"

#include <chrono>
#include <cstddef>

namespace ou::http {

inline constexpr std::chrono::milliseconds kPollTimeout{ 100 };
inline constexpr std::chrono::milliseconds kAcceptRetryDelay{ 10 };

// What a connection loop borrows from its Transport, which outlives it
struct LoopContext {
	int listenSocket = -1;
	size_t maxRequestBytes = 0;
	std::chrono::seconds idleTimeout{};
	SocketIo &io;
	const Dispatcher &dispatcher;
	ThreadPool *pool = nullptr; // Event loop only; null runs offload() inline
};

} // namespace ou::http
