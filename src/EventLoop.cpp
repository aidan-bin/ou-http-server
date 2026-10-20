#include "EventLoop.h"
#include "ou/http/Logging.h"

#include <algorithm>
#include <utility>

#include <sys/socket.h>

namespace {

constexpr auto kPollTimeout = std::chrono::milliseconds(100);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local ou::http::EventLoop *g_currentLoop = nullptr;

} // namespace

namespace ou::http {

EventLoop::EventLoop(Config config, SocketHandler &socketHandler, Dispatch dispatch, Reject reject, HandlerPool *pool)
		: config_(config), socketHandler_(socketHandler), dispatch_(std::move(dispatch)), reject_(std::move(reject)), pool_(pool) {}

void EventLoop::run(const std::stop_token &stop) {
	if (!poller_.valid()) {
		logError("Failed to create the event poller");
		return;
	}

	if (!poller_.add(config_.listenSocket, Interest::Read)) {
		logError("Failed to watch listening socket {}", config_.listenSocket);
		return;
	}

	if (pool_ != nullptr && !poller_.enableWake()) {
		logError("Failed to set up handler pool wakeups");
		return;
	}

	g_currentLoop = this;
	while (!stop.stop_requested()) {
		for (const ReadyEvent &event : poller_.wait(pollTimeout())) {
			if (event.woken) {
				resumeFinishedJobs();
				continue;
			}

			if (event.fd == config_.listenSocket) {
				acceptReady();
				continue;
			}

			const auto connection = connections_.find(event.fd);
			if (connection != connections_.end())
				serviceOrClose(connection);
		}

		resumeDueTimers();
		sweepIdle();
	}

	while (!connections_.empty()) {
		close(connections_.begin());
	}
	g_currentLoop = nullptr;

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

		const auto connection = connections_.try_emplace(clientSocket, std::move(client), address, config_.maxRequestBytes).first;

		// Start the handshake now rather than waiting for a wakeup that a TLS client will not send
		serviceOrClose(connection);
	}
}

void EventLoop::serviceOrClose(ConnectionMap::iterator connection) {
	bool keepOpen = false;
	try {
		keepOpen = service(connection->second);
	} catch (const std::exception &e) {
		logError("Unhandled exception while serving client: {}", e.what());
	} catch (...) {
		logError("Unhandled exception of unknown type while serving client");
	}

	if (!keepOpen)
		close(connection);
}

bool EventLoop::service(Connection &connection) {
	connection.lastActivity = std::chrono::steady_clock::now();

	switch (connection.state) {
		using enum State;
	case Handshaking:
		return handshake(connection);
	case Reading:
		return readFrom(connection);
	case Suspended:
		return respondIfDone(connection);
	case Writing:
		return writeTo(connection);
	}
	std::unreachable();
}

bool EventLoop::handshake(Connection &connection) {
	const IoStatus status = socketHandler_.handshake(connection.fd.get()).status;
	if (status != IoStatus::Ok)
		return rearm(connection, status);

	connection.state = State::Reading;
	return readFrom(connection);
}

bool EventLoop::readFrom(Connection &connection) {
	const IoResult result = socketHandler_.read(connection.fd.get(), scratch_);
	if (result.status != IoStatus::Ok)
		return rearm(connection, result.status);

	return handleReaderStatus(connection, connection.reader.consume({ scratch_.data(), result.bytes }));
}

bool EventLoop::handleReaderStatus(Connection &connection, RequestReader::Status status) {
	switch (status) {
		using enum RequestReader::Status;
	case NeedMore:
		connection.state = State::Reading;
		return arm(connection, Interest::Read);

	case Rejected:
		return respond(connection, reject_(connection.reader.rejectStatus(), connection.address));

	case Complete:
		break;
	}

	return start(connection);
}

bool EventLoop::start(Connection &connection) {
	connection.request = connection.reader.take();
	connection.generation = nextGeneration_++;
	runningRequest_ = { .fd = connection.fd.get(), .generation = connection.generation };

	Task<Response> task = dispatch_(connection.request, connection.address);
	if (task.done())
		return respond(connection, task.result());

	// Level-triggered: pipelined bytes or a hangup would otherwise wake the loop until the response is sent
	if (!poller_.remove(connection.fd.get()))
		return false;

	connection.state = State::Suspended;
	connection.task.emplace(std::move(task));
	return true;
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

		if (result.status != IoStatus::Ok)
			return rearm(connection, result.status);
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

	if (!poller_.modify(connection.fd.get(), interest))
		return false;

	connection.interest = interest;
	return true;
}

bool EventLoop::rearm(Connection &connection, IoStatus status) {
	if (status == IoStatus::WantRead)
		return arm(connection, Interest::Read);
	if (status == IoStatus::WantWrite)
		return arm(connection, Interest::Write);
	return false;
}

bool EventLoop::respondIfDone(Connection &connection) {
	if (!connection.task || !connection.task->done())
		return true;

	Task<Response> task = std::move(*connection.task);
	connection.task.reset();

	if (!poller_.add(connection.fd.get(), Interest::Read))
		return false;
	connection.interest = Interest::Read;
	return respond(connection, task.result()); // Rethrows a handler's exception into serviceOrClose
}

EventLoop *EventLoop::current() noexcept { return g_currentLoop; }

void EventLoop::resumeAt(std::chrono::steady_clock::time_point deadline, std::coroutine_handle<> coroutine) {
	timers_.emplace(deadline, Resumption{ .request = runningRequest_, .coroutine = coroutine });
}

void EventLoop::resumeAfter(std::function<void()> work, std::coroutine_handle<> coroutine) {
	pool_->submit([this, work = std::move(work), resumption = Resumption{ .request = runningRequest_, .coroutine = coroutine }] {
		work();
		completions_.push(resumption);
	});
}

void EventLoop::resume(const Resumption &resumption) {
	// Closed (its fd maybe reused) or already answered: the coroutine is gone
	const auto connection = connections_.find(resumption.request.fd);
	if (connection == connections_.end() || connection->second.state != State::Suspended
			|| connection->second.generation != resumption.request.generation)
		return;

	runningRequest_ = resumption.request;
	resumption.coroutine.resume();
	serviceOrClose(connection);
}

void EventLoop::resumeDueTimers() {
	const auto now = std::chrono::steady_clock::now();
	while (!timers_.empty() && timers_.begin()->first <= now) {
		const Resumption due = timers_.begin()->second;
		timers_.erase(timers_.begin());
		resume(due);
	}
}

void EventLoop::resumeFinishedJobs() {
	for (const Resumption &finished : completions_.takeAll()) {
		resume(finished);
	}
}

std::chrono::milliseconds EventLoop::pollTimeout() const {
	if (timers_.empty())
		return kPollTimeout;

	const auto untilDue = std::chrono::ceil<std::chrono::milliseconds>(timers_.begin()->first - std::chrono::steady_clock::now());
	return std::clamp(untilDue, std::chrono::milliseconds(0), kPollTimeout);
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
		if (connection->second.state == State::Suspended || now - connection->second.lastActivity < config_.idleTimeout) {
			++connection;
			continue;
		}

		logWarn("Closing idle connection from {}", addressToString(connection->second.address));
		connection = close(connection);
	}
}

} // namespace ou::http
