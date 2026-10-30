#include "BlockingLoop.h"
#include "Listener.h"
#include "UniqueFd.h"
#include "ou/http/Logging.h"

#include <array>
#include <cerrno>
#include <exception>
#include <expected>
#include <system_error>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace ou::http {

BlockingLoop::BlockingLoop(LoopContext context) : context_(context) {}

UniqueFd BlockingLoop::acceptOne(sockaddr_in &clientAddr) const {
	pollfd descriptor{ .fd = context_.listenSocket, .events = POLLIN, .revents = 0 };

	const int ready = ::poll(&descriptor, 1, static_cast<int>(kPollTimeout.count()));
	if (ready == 0)
		return {};

	if (ready < 0) {
		if (errno != EINTR)
			std::this_thread::sleep_for(kAcceptRetryDelay);
		return {};
	}

	std::expected<UniqueFd, std::error_code> client = acceptClient(context_.listenSocket, clientAddr);
	if (!client) {
		log::warn("Failed to accept connection: {}", client.error().message());
		std::this_thread::sleep_for(kAcceptRetryDelay);
		return {};
	}

	// macOS/BSD accept() copies O_NONBLOCK from the listener; this model needs blocking reads that time out
	(void)setNonBlocking(client->get(), false);
	return std::move(*client);
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
			log::error("Unhandled exception while serving client: {}", e.what());
		} catch (...) {
			log::error("Unhandled exception of unknown type while serving client");
		}

		context_.io.discardSession(clientSocket.get()); // UniqueFd closes the fd
	}
}

void BlockingLoop::handleConnection(int clientSocket, const sockaddr_in &clientAddr, const std::stop_token &stop) const {
	// Bounds every blocking read and write (a stalled peer releases this thread after idleTimeout)
	const timeval timeout{ .tv_sec = static_cast<time_t>(context_.idleTimeout.count()), .tv_usec = 0 };
	(void)setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	if (context_.io.handshake(clientSocket).status != IoStatus::Ok)
		return;

	RequestReader reader(context_.maxRequestBytes);

	while (!stop.stop_requested()) {
		Response response;
		switch (readRequest(clientSocket, reader)) {
			using enum RequestReader::Status;
		case NeedMore:
			// The peer went away with nothing to answer
			return;
		case Rejected:
			response = context_.dispatcher.reject(reader.rejectStatus(), clientAddr);
			break;
		case Complete: {
			Request request = reader.take();
			response = context_.dispatcher.serve(request, clientAddr).result();
			break;
		}
		}

		if (context_.io.writeAll(clientSocket, response.serialize()).status != IoStatus::Ok)
			return;

		if (!response.keepAlive)
			return;
	}
}

RequestReader::Status BlockingLoop::readRequest(int clientSocket, RequestReader &reader) const {
	std::array<char, RequestReader::kChunkBytes> chunk{};

	RequestReader::Status status = reader.next();

	while (status == RequestReader::Status::NeedMore) {
		const IoResult result = context_.io.read(clientSocket, chunk);
		if (result.status != IoStatus::Ok)
			return reader.endOfInput(); // Timed out or the peer went away

		status = reader.consume({ chunk.data(), result.bytes });
	}

	return status;
}

} // namespace ou::http
