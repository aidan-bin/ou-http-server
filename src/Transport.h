#pragma once

#include "ConnectionLoop.h"
#include "HandlerPool.h"
#include "Listener.h"
#include "SocketHandler.h"
#include "UniqueFd.h"
#include "ou/http/Server.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <thread>
#include <vector>

namespace ou::http {

// Accepts connections and moves bytes for one start-to-stop cycle; knows nothing about routes or files.
// Owns the listening socket, the plain or TLS socket handler, one connection loop per thread, and the
// event-loop model's offload pool (stopped for good, so it lives and dies with the loops that use it).
class Transport {
public:
	// Fails on a config that could never serve, or if the port cannot be opened; else starts config.threadCount loops
	[[nodiscard]] static std::expected<std::unique_ptr<Transport>, Error> start(const Server::Config &config, Dispatch dispatch,
																																							Reject reject);

	// Use start(): this takes the resources it has already opened
	Transport(const Server::Config &config, std::unique_ptr<SocketHandler> socketHandler, Listener listener, Dispatch dispatch,
						Reject reject);
	Transport(const Transport &) = delete;
	Transport &operator=(const Transport &) = delete;
	Transport(Transport &&) = delete;
	Transport &operator=(Transport &&) = delete;

	// Stops the handler pool first, then the loops, then closes the listening socket
	~Transport();

	[[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
	void run(const std::stop_token &stop);

	UniqueFd listenSocket_; // Shared by every loop
	uint16_t port_;
	std::unique_ptr<SocketHandler> socketHandler_;
	std::unique_ptr<HandlerPool> pool_; // Event loop only
	Dispatch dispatch_;
	Reject reject_;
	Server::Model model_;
	LoopConfig loopConfig_;
	std::vector<std::jthread> threads_; // Last, so they join before anything they use is destroyed
};

} // namespace ou::http
