#pragma once

#include "CompletionQueue.h"
#include "LoopContext.h"
#include "Poller.h"
#include "RequestReader.h"
#include "SocketIo.h"
#include "ThreadPool.h"
#include "UniqueFd.h"
#include "ou/http/HttpTypes.h"
#include "ou/http/Task.h"

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>

#include <netinet/in.h>

namespace ou::http {

// One per worker thread; owns each connection
class EventLoop {
public:
	explicit EventLoop(LoopContext context);

	// Returns within one poll timeout of a stop request
	void run(const std::stop_token &stop);

	// Finds the loop running on this thread; null on threads without one
	[[nodiscard]] static EventLoop *current() noexcept;

	// Resumes the running request's coroutine at deadline; dropped if the request ends first
	void resumeAt(std::chrono::steady_clock::time_point deadline, std::coroutine_handle<> coroutine);

	// Runs work on the offload pool, then resumes the running request's coroutine here; needs hasPool()
	void offloadThenResume(std::function<void()> work, std::coroutine_handle<> coroutine);

	[[nodiscard]] bool hasPool() const noexcept { return context_.pool != nullptr; }

private:
	enum class State : uint8_t { Handshaking, Reading, Suspended, Writing };

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
		bool keepAlive = false;

		// The request in flight; its task may hold a reference to it, so it is declared first and outlives it
		Request request;
		std::optional<Task<Response>> task; // While Suspended
		uint64_t generation = 0;
	};

	using ConnectionMap = std::unordered_map<int, Connection>;

	enum class Next : uint8_t { Continue, Wait, Close };

	void acceptReady();

	// Steps the connection until it has to wait; closes it when it is done or a step throws
	void advanceOrClose(ConnectionMap::iterator connection);
	[[nodiscard]] Next step(Connection &connection);

	[[nodiscard]] Next handshake(Connection &connection);
	[[nodiscard]] Next read(Connection &connection);
	[[nodiscard]] Next collectResponse(Connection &connection);
	[[nodiscard]] Next write(Connection &connection);

	[[nodiscard]] Next afterRead(Connection &connection, RequestReader::Status status);

	[[nodiscard]] Next runHandler(Connection &connection);
	static void beginWrite(Connection &connection, const Response &response);

	[[nodiscard]] Next waitFor(Connection &connection, Interest interest);
	// Waits for the readiness a non-Ok status asks for; closes on Closed or Error
	[[nodiscard]] Next waitFor(Connection &connection, IoStatus status);

	// Resumes a suspended request's coroutine; dropped if the request already ended
	void resume(const Resumption &resumption);
	void resumeDueTimers();
	void resumeFinishedJobs();

	// Waits no longer than the next timer
	[[nodiscard]] std::chrono::milliseconds pollTimeout() const;

	// Returns the following connection for iteration
	ConnectionMap::iterator close(ConnectionMap::iterator connection);

	// Closes idle connections
	void sweepIdle();

	void pauseAccepting();
	void resumeAccepting();

	LoopContext context_;

	Poller poller_;
	CompletionQueue completions_{ poller_ };
	std::multimap<std::chrono::steady_clock::time_point, Resumption> timers_;
	ConnectionMap connections_;
	std::optional<std::chrono::steady_clock::time_point> acceptPausedUntil_;
	uint64_t nextGeneration_ = 0;
	// The request whose handler is running, for awaiters to tag resumptions with; fd -1 between runs
	RequestId runningRequest_;
	std::array<char, RequestReader::kChunkBytes> scratch_{};
};

} // namespace ou::http
