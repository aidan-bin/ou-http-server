#include "EventLoop.h"
#include "Logging.h"

#include <format>
#include <utility>

#include <sys/socket.h>

namespace {

constexpr auto kPollTimeout = std::chrono::milliseconds(100);

} // namespace

namespace ou::http {

EventLoop::EventLoop(Config config, SocketHandler &socketHandler, Dispatch dispatch)
		: config_(config), socketHandler_(socketHandler), dispatch_(std::move(dispatch)) {}

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
		if (!client)
			return; // Drained, or a failure the next wakeup will retry

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
		const IoResult accepted = socketHandler_.acceptConnection(connection.fd.get());
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

	switch (connection.reader.consume({ scratch_.data(), result.bytes })) {
	case RequestReader::Status::NeedMore:
		return arm(connection, Interest::Read);

	case RequestReader::Status::Rejected:
		LOG_WARN("Rejecting request from {} with status {}", addressToString(connection.address), connection.reader.rejectStatus());
		return respond(connection, Response::error(connection.reader.rejectStatus()));

	case RequestReader::Status::Complete:
		break;
	}

	Request request = connection.reader.take();
	request.clientAddr = connection.address;

	const std::string requestLine = std::format("{} {}", request.method, request.path);
	const Response response = dispatch_(std::move(request));
	LOG_INFO("{} from {} -> {}", requestLine, addressToString(connection.address), response.statusCode);

	return respond(connection, response);
}

bool EventLoop::respond(Connection &connection, const Response &response) {
	connection.outBuffer = response.serialize();
	connection.written = 0;
	connection.state = State::Writing;

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

	// One request per connection, so a fully written response is the end of it
	return false;
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

void EventLoop::close(ConnectionMap::iterator connection) {
	const int clientSocket = connection->second.fd.get();

	(void)poller_.remove(clientSocket);
	(void)connection->second.fd.release(); // The handler closes it and frees any TLS session
	socketHandler_.closeConnection(clientSocket);

	connections_.erase(connection);
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
		close(connection++);
	}
}

} // namespace ou::http
