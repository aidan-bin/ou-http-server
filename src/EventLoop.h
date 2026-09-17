#pragma once

#include "HttpTypes.h"
#include "Poller.h"
#include "RequestReader.h"
#include "SocketHandler.h"
#include "UniqueFd.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include <netinet/in.h>

namespace ou::http {

// One per worker thread; owns each connection
class EventLoop {
public:
	using Dispatch = std::function<Response(Request)>;

	struct Config {
		int listenSocket = -1; // Borrowed from Server
		size_t maxRequestBytes = size_t{ 1024 } * 1024;
		std::chrono::seconds idleTimeout{ 10 };
	};

	EventLoop(Config config, SocketHandler &socketHandler, Dispatch dispatch);

	// Returns within one poll timeout of running going false
	void run(const std::atomic<bool> &running);

private:
	enum class State : uint8_t { Handshaking, Reading, Writing };

	struct Connection {
		Connection(UniqueFd socket, const sockaddr_in &peer, size_t maxRequestBytes)
				: fd(std::move(socket)), address(peer), reader(maxRequestBytes), lastActivity(std::chrono::steady_clock::now()) {}

		UniqueFd fd;
		sockaddr_in address{};
		RequestReader reader;
		std::chrono::steady_clock::time_point lastActivity;
		State state = State::Handshaking;
		Interest interest = Interest::Read;
		std::string outBuffer;
		size_t written = 0;
	};

	using ConnectionMap = std::unordered_map<int, Connection>;

	void acceptReady();

	// False means the connection is finished (success or failure); caller should close it
	[[nodiscard]] bool service(Connection &connection);
	[[nodiscard]] bool readFrom(Connection &connection);
	[[nodiscard]] bool writeTo(Connection &connection);
	[[nodiscard]] bool respond(Connection &connection, const Response &response);
	[[nodiscard]] bool arm(Connection &connection, Interest interest);

	void serviceOrClose(ConnectionMap::iterator connection);

	void close(ConnectionMap::iterator connection);
	void sweepIdle();

	Config config_;
	SocketHandler &socketHandler_;
	Dispatch dispatch_;

	Poller poller_;
	ConnectionMap connections_;
	std::array<char, RequestReader::kChunkBytes> scratch_{};
};

} // namespace ou::http
