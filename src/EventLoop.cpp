#include "EventLoop.h"
#include "Listener.h"
#include "ou/http/Logging.h"

#include <algorithm>
#include <expected>
#include <system_error>
#include <utility>

namespace ou::http {

namespace {

	thread_local EventLoop *g_currentLoop = nullptr;

} // namespace

EventLoop::EventLoop(LoopContext context) : context_(context) {}

void EventLoop::run(const std::stop_token &stop) {
	if (!poller_.valid()) {
		log::error("Failed to create the event poller");
		return;
	}

	if (!poller_.add(context_.listenSocket, Interest::Read)) {
		log::error("Failed to watch listening socket {}", context_.listenSocket);
		return;
	}

	if (context_.pool != nullptr && !poller_.enableWake()) {
		log::error("Failed to set up offload pool wakeups");
		return;
	}

	g_currentLoop = this;
	while (!stop.stop_requested()) {
		for (const ReadyEvent &event : poller_.wait(pollTimeout())) {
			if (event.woken) {
				resumeFinishedJobs();
				continue;
			}

			if (event.fd == context_.listenSocket) {
				acceptReady();
				continue;
			}

			const auto connection = connections_.find(event.fd);
			if (connection != connections_.end())
				advanceOrClose(connection);
		}

		resumeDueTimers();
		sweepIdle();
		resumeAccepting();
	}

	while (!connections_.empty()) {
		close(connections_.begin());
	}
	g_currentLoop = nullptr;

	(void)poller_.remove(context_.listenSocket);
}

void EventLoop::acceptReady() {
	while (true) {
		sockaddr_in address{};

		std::expected<UniqueFd, std::error_code> client = acceptClient(context_.listenSocket, address);
		if (!client) {
			log::warn("Failed to accept connection: {}", client.error().message());
			pauseAccepting();
			return;
		}
		if (!client->valid())
			return;

		const int clientSocket = client->get();
		if (!setNonBlocking(clientSocket, true) || !poller_.add(clientSocket, Interest::Read)) {
			log::warn("Failed to set up connection from {}", addressToString(address));
			continue;
		}

		const auto connection = connections_.try_emplace(clientSocket, std::move(*client), address, context_.maxRequestBytes).first;

		// Start the handshake now rather than waiting for a wakeup that a TLS client will not send
		advanceOrClose(connection);
	}
}

void EventLoop::advanceOrClose(ConnectionMap::iterator connection) {
	connection->second.lastActivity = std::chrono::steady_clock::now();

	Next next = Next::Continue;
	try {
		while (next == Next::Continue)
			next = step(connection->second);
	} catch (const std::exception &e) {
		log::error("Unhandled exception while serving client: {}", e.what());
		next = Next::Close;
	} catch (...) {
		log::error("Unhandled exception of unknown type while serving client");
		next = Next::Close;
	}

	if (next == Next::Close)
		close(connection);
}

EventLoop::Next EventLoop::step(Connection &connection) {
	switch (connection.state) {
		using enum State;
	case Handshaking:
		return handshake(connection);
	case Reading:
		return read(connection);
	case Suspended:
		return collectResponse(connection);
	case Writing:
		return write(connection);
	}
	std::unreachable();
}

EventLoop::Next EventLoop::handshake(Connection &connection) {
	const IoStatus status = context_.io.handshake(connection.fd.get()).status;
	if (status != IoStatus::Ok)
		return waitFor(connection, status);

	connection.state = State::Reading;
	return Next::Continue;
}

EventLoop::Next EventLoop::read(Connection &connection) {
	const IoResult result = context_.io.read(connection.fd.get(), scratch_);
	if (result.status != IoStatus::Ok)
		return waitFor(connection, result.status);

	return afterRead(connection, connection.reader.consume({ scratch_.data(), result.bytes }));
}

EventLoop::Next EventLoop::collectResponse(Connection &connection) {
	if (!connection.task || !connection.task->done())
		return Next::Wait;

	Task<Response> task = std::move(*connection.task);
	connection.task.reset();

	if (!poller_.add(connection.fd.get(), Interest::Read))
		return Next::Close;
	connection.interest = Interest::Read;

	beginWrite(connection, task.result());
	return Next::Continue;
}

EventLoop::Next EventLoop::write(Connection &connection) {
	while (connection.written < connection.outBuffer.size()) {
		const std::string_view remaining = std::string_view(connection.outBuffer).substr(connection.written);

		const IoResult result = context_.io.write(connection.fd.get(), remaining);
		connection.written += result.bytes;

		if (result.status != IoStatus::Ok)
			return waitFor(connection, result.status);
	}

	if (!connection.keepAlive)
		return Next::Close;

	connection.outBuffer.clear();
	connection.written = 0;
	return afterRead(connection, connection.reader.next());
}

EventLoop::Next EventLoop::afterRead(Connection &connection, RequestReader::Status status) {
	switch (status) {
		using enum RequestReader::Status;
	case NeedMore:
		connection.state = State::Reading;
		// TLS may hold decrypted bytes the poller cannot see
		if (context_.io.hasBuffered(connection.fd.get()))
			return Next::Continue;
		return waitFor(connection, Interest::Read);

	case Rejected:
		beginWrite(connection, context_.dispatcher.reject(connection.reader.rejectStatus(), connection.address));
		return Next::Continue;

	case Complete:
		return runHandler(connection);
	}
	std::unreachable();
}

EventLoop::Next EventLoop::runHandler(Connection &connection) {
	connection.request = connection.reader.take();
	connection.generation = nextGeneration_++;

	runningRequest_ = RequestId{ .fd = connection.fd.get(), .generation = connection.generation };
	Task<Response> task = context_.dispatcher.serve(connection.request, connection.address);
	runningRequest_ = {};

	if (task.done()) {
		beginWrite(connection, task.result());
		return Next::Continue;
	}

	// Level-triggered: pipelined bytes or a hangup would otherwise wake the loop until the response is sent
	if (!poller_.remove(connection.fd.get()))
		return Next::Close;

	connection.state = State::Suspended;
	connection.task.emplace(std::move(task));
	return Next::Wait;
}

void EventLoop::beginWrite(Connection &connection, const Response &response) {
	connection.outBuffer = response.serialize();
	connection.written = 0;
	connection.keepAlive = response.keepAlive;
	connection.state = State::Writing;
}

EventLoop::Next EventLoop::waitFor(Connection &connection, Interest interest) {
	if (connection.interest != interest) {
		if (!poller_.modify(connection.fd.get(), interest))
			return Next::Close;
		connection.interest = interest;
	}
	return Next::Wait;
}

EventLoop::Next EventLoop::waitFor(Connection &connection, IoStatus status) {
	if (status == IoStatus::WantRead)
		return waitFor(connection, Interest::Read);
	if (status == IoStatus::WantWrite)
		return waitFor(connection, Interest::Write);
	return Next::Close;
}

EventLoop *EventLoop::current() noexcept { return g_currentLoop; }

void EventLoop::resumeAt(std::chrono::steady_clock::time_point deadline, std::coroutine_handle<> coroutine) {
	timers_.emplace(deadline, Resumption{ .request = runningRequest_, .coroutine = coroutine });
}

void EventLoop::offloadThenResume(std::function<void()> work, std::coroutine_handle<> coroutine) {
	context_.pool->submit([this, work = std::move(work), resumption = Resumption{ .request = runningRequest_, .coroutine = coroutine }] {
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
	runningRequest_ = {};
	advanceOrClose(connection);
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
	context_.io.discardSession(clientSocket);

	return connections_.erase(connection); // Closes connection in the UniqueFd destructor
}

void EventLoop::sweepIdle() {
	// TODO: linear scan per wakeup; a timer wheel if connection counts ever make it show up
	const auto now = std::chrono::steady_clock::now();

	for (auto connection = connections_.begin(); connection != connections_.end();) {
		if (connection->second.state == State::Suspended || now - connection->second.lastActivity < context_.idleTimeout) {
			++connection;
			continue;
		}

		const bool midRequest = connection->second.reader.endOfInput() == RequestReader::Status::Rejected;
		if (midRequest)
			log::warn("Closing idle connection from {}", addressToString(connection->second.address));
		connection = close(connection);
	}
}

void EventLoop::pauseAccepting() {
	if (poller_.remove(context_.listenSocket))
		acceptPausedUntil_ = std::chrono::steady_clock::now() + kAcceptRetryDelay;
}

void EventLoop::resumeAccepting() {
	if (!acceptPausedUntil_ || std::chrono::steady_clock::now() < *acceptPausedUntil_)
		return;
	if (poller_.add(context_.listenSocket, Interest::Read))
		acceptPausedUntil_.reset();
}

} // namespace ou::http
