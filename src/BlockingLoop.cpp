#include "BlockingLoop.h"
#include "UniqueFd.h"
#include "ou/http/Logging.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

constexpr int kAcceptPollMs = 200;
constexpr auto kAcceptRetryDelay = std::chrono::milliseconds(10);

// Not failures: another worker took the connection, the client hung up while queued, or a signal
bool sIsBenignAcceptError(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == ECONNABORTED || error == EINTR; }

} // namespace

namespace ou::http {

BlockingLoop::BlockingLoop(Config config, SocketHandler &socketHandler, Dispatch dispatch, Reject reject)
		: config_(config), socketHandler_(socketHandler), dispatch_(std::move(dispatch)), reject_(std::move(reject)) {}

UniqueFd BlockingLoop::acceptOne(sockaddr_in &clientAddr) const {
	pollfd descriptor{ .fd = config_.listenSocket, .events = POLLIN, .revents = 0 };

	const int ready = ::poll(&descriptor, 1, kAcceptPollMs);
	if (ready == 0)
		return {};

	if (ready < 0) {
		if (errno != EINTR)
			std::this_thread::sleep_for(kAcceptRetryDelay);
		return {};
	}

	socklen_t clientLen = sizeof(clientAddr);
	UniqueFd clientSocket(::accept(config_.listenSocket, reinterpret_cast<struct sockaddr *>(&clientAddr), &clientLen));
	if (!clientSocket) {
		// Back off on real errors so a persistent one can't spin this thread
		if (!sIsBenignAcceptError(errno)) {
			logWarn("Failed to accept connection: errno {}", errno);
			std::this_thread::sleep_for(kAcceptRetryDelay);
		}
		return {};
	}

	// macOS/BSD accept() copies O_NONBLOCK from the listener; this model needs blocking reads that time out
	(void)setNonBlocking(clientSocket.get(), false);
	return clientSocket;
}

void BlockingLoop::run(const std::stop_token &stop) {
	while (!stop.stop_requested()) {
		sockaddr_in clientAddr{};

		const UniqueFd clientSocket = acceptOne(clientAddr);
		if (!clientSocket)
			continue;

		try {
			handleConnection(clientSocket.get(), clientAddr, stop);
		} catch (const std::exception &e) {
			logError("Unhandled exception while serving client: {}", e.what());
		} catch (...) {
			logError("Unhandled exception of unknown type while serving client");
		}

		socketHandler_.discardSession(clientSocket.get()); // UniqueFd closes the fd
	}
}

void BlockingLoop::handleConnection(int clientSocket, const sockaddr_in &clientAddr, const std::stop_token &stop) const {
	// Bounds every blocking read and write (a stalled peer releases this thread after idleTimeout)
	const timeval timeout{ .tv_sec = static_cast<time_t>(config_.idleTimeout.count()), .tv_usec = 0 };
	(void)setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	if (socketHandler_.handshake(clientSocket).status != IoStatus::Ok)
		return;

	RequestReader reader(config_.maxRequestBytes);

	while (!stop.stop_requested()) {
		Response response;
		switch (readRequest(clientSocket, reader)) {
			using enum RequestReader::Status;
		case NeedMore:
			// The peer went away with nothing to answer
			return;
		case Rejected:
			response = reject_(reader.rejectStatus(), clientAddr);
			break;
		case Complete: {
			Request request = reader.take();
			response = dispatch_(request, clientAddr).result();
			break;
		}
		}

		if (socketHandler_.writeAll(clientSocket, response.serialize()).status != IoStatus::Ok)
			return;

		if (!response.keepAlive)
			return;
	}
}

RequestReader::Status BlockingLoop::readRequest(int clientSocket, RequestReader &reader) const {
	std::array<char, RequestReader::kChunkBytes> chunk{};

	RequestReader::Status status = reader.reset();

	while (status == RequestReader::Status::NeedMore) {
		const IoResult result = socketHandler_.read(clientSocket, chunk);
		if (result.status != IoStatus::Ok)
			return reader.abandon(); // Timed out or the peer went away

		status = reader.consume({ chunk.data(), result.bytes });
	}

	return status;
}

} // namespace ou::http
