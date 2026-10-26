#pragma once

#include "LoopConfig.h"
#include "RequestReader.h"
#include "SocketHandler.h"
#include "UniqueFd.h"

#include <stop_token>

#include <netinet/in.h>

namespace ou::http {

// One per worker thread, serving one blocking connection at a time: a keep-alive connection holds
// its thread until it closes, so threadCount caps how many clients can be served at once
class BlockingLoop {
public:
	using Config = LoopConfig;

	BlockingLoop(Config config, SocketHandler &socketHandler, Dispatch dispatch, Reject reject);

	// Returns within one accept poll timeout of a stop request
	void run(const std::stop_token &stop);

private:
	// Empty on timeout or error. Polls rather than blocking in accept() so the worker notices stop() on
	// its own; see ~Transport
	[[nodiscard]] UniqueFd acceptOne(sockaddr_in &clientAddr) const;

	// Never throws, and never closes the socket
	void handleConnection(int clientSocket, const sockaddr_in &clientAddr, const std::stop_token &stop) const;

	// Reads until a request is complete, unacceptable, or the peer stops sending
	[[nodiscard]] RequestReader::Status readRequest(int clientSocket, RequestReader &reader) const;

	Config config_;
	SocketHandler &socketHandler_;
	Dispatch dispatch_;
	Reject reject_;
};

} // namespace ou::http
