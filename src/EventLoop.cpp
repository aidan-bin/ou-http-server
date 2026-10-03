#include "EventLoop.h"
#include "Logging.h"

#include <utility>

#include <sys/socket.h>

namespace {

constexpr auto kPollTimeout = std::chrono::milliseconds(100);

} // namespace

namespace ou::http {

EventLoop::EventLoop(Config config, SocketHandler &socketHandler, Dispatch dispatch, Reject reject)
		: config_(config), socketHandler_(socketHandler), dispatch_(std::move(dispatch)), reject_(std::move(reject)) {}

void EventLoop::run(const std::atomic<bool> &running) {
	if (!poller_.valid()) {
		LOG_ERROR("Failed to create the event poller");
		return;
	}

	if (!poller_.add(config_.listenSocket, Interest::Read)) {
		LOG_ERROR("Failed to watch listening socket {}", config_.listenSocket);
		return;
	}

	while (running.load()) {
		for (const ReadyEvent &event : poller_.wait(kPollTimeout)) {
			if (event.fd == config_.listenSocket) {
				acceptReady();
				continue;
			}

			const auto connection = connections_.find(event.fd);
			if (connection != connections_.end())
				serviceOrClose(connection);
		}

		sweepIdle();
	}

	while (!connections_.empty()) {
		close(connections_.begin());
	}

	(void)poller_.remove(config_.listenSocket);
}

void EventLoop::acceptReady() {
	while (true) {
		sockaddr_in address{};
		socklen_t length = sizeof(address);

		UniqueFd client(::accept(config_.listenSocket, reinterpret_cast<struct sockaddr *>(&address), &length));
		// EAGAIN or a failed accept -> stop
		if (!client)
			return;

		if (!setNonBlocking(client.get(), true))
			continue;

		const int clientSocket = client.get();
		if (!poller_.add(clientSocket, Interest::Read))
			continue;

		const auto [connection, inserted] = connections_.try_emplace(clientSocket, std::move(client), address, config_.maxRequestBytes);
		if (!inserted)
			continue;

		// Start the handshake now rather than waiting for a wakeup that a TLS client will not send
		serviceOrClose(connection);
	}
}

void EventLoop::serviceOrClose(ConnectionMap::iterator connection) {
	bool keepOpen = false;
	try {
		keepOpen = service(connection->second);
	} catch (const std::exception &e) {
		LOG_ERROR("Unhandled exception while serving client: {}", e.what());
	} catch (...) {
		LOG_ERROR("Unhandled exception of unknown type while serving client");
	}

	if (!keepOpen)
		close(connection);
}

bool EventLoop::service(Connection &connection) {
	connection.lastActivity = std::chrono::steady_clock::now();

	if (connection.state == State::Handshaking) {
		const IoResult accepted = socketHandler_.handshake(connection.fd.get());
		if (accepted.status == IoStatus::WantRead)
			return arm(connection, Interest::Read);
		if (accepted.status == IoStatus::WantWrite)
			return arm(connection, Interest::Write);
		if (accepted.status != IoStatus::Ok)
			return false;

		connection.state = State::Reading;
	}

	return connection.state == State::Reading ? readFrom(connection) : writeTo(connection);
}

bool EventLoop::readFrom(Connection &connection) {
	const IoResult result = socketHandler_.read(connection.fd.get(), scratch_);
	if (result.status == IoStatus::WantRead)
		return arm(connection, Interest::Read);
	if (result.status == IoStatus::WantWrite)
		return arm(connection, Interest::Write);
	if (result.status != IoStatus::Ok)
		return false;

	return handleReaderStatus(connection, connection.reader.consume({ scratch_.data(), result.bytes }));
}

bool EventLoop::handleReaderStatus(Connection &connection, RequestReader::Status status) {
	switch (status) {
	case RequestReader::Status::NeedMore:
		connection.state = State::Reading;
		return arm(connection, Interest::Read);

	case RequestReader::Status::Rejected:
		return respond(connection, reject_(connection.reader.rejectStatus(), connection.address));

	case RequestReader::Status::Complete:
		break;
	}

	return respond(connection, dispatch_(connection.reader.take(), connection.address));
}

bool EventLoop::respond(Connection &connection, const Response &response) {
	connection.outBuffer = response.serialize();
	connection.written = 0;
	connection.state = State::Writing;
	connection.keepAlive = response.keepAlive;

	// Try immediately: the send buffer is almost always empty, which saves a wakeup per request
	return writeTo(connection);
}

bool EventLoop::writeTo(Connection &connection) {
	while (connection.written < connection.outBuffer.size()) {
		const std::string_view remaining = std::string_view(connection.outBuffer).substr(connection.written);

		const IoResult result = socketHandler_.write(connection.fd.get(), remaining);
		connection.written += result.bytes;

		if (result.status == IoStatus::WantRead)
			return arm(connection, Interest::Read);
		if (result.status == IoStatus::WantWrite)
			return arm(connection, Interest::Write);
		if (result.status != IoStatus::Ok)
			return false;
	}

	if (!connection.keepAlive)
		return false;

	connection.outBuffer.clear();
	connection.written = 0;
	connection.state = State::Reading;

	return handleReaderStatus(connection, connection.reader.reset());
}

bool EventLoop::arm(Connection &connection, Interest interest) {
	if (connection.interest == interest)
		return true;

	// Connection interest has changed; update poller
	if (!poller_.modify(connection.fd.get(), interest))
		return false;

	connection.interest = interest;
	return true;
}

EventLoop::ConnectionMap::iterator EventLoop::close(ConnectionMap::iterator connection) {
	const int clientSocket = connection->second.fd.get();

	(void)poller_.remove(clientSocket);
	socketHandler_.discardSession(clientSocket);

	return connections_.erase(connection); // Closes connection in the UniqueFd destructor
}

void EventLoop::sweepIdle() {
	// TODO: linear scan per wakeup; a timer wheel if connection counts ever make it show up
	const auto now = std::chrono::steady_clock::now();

	for (auto connection = connections_.begin(); connection != connections_.end();) {
		if (now - connection->second.lastActivity < config_.idleTimeout) {
			++connection;
			continue;
		}

		LOG_WARN("Closing idle connection from {}", addressToString(connection->second.address));
		connection = close(connection);
	}
}

} // namespace ou::http
