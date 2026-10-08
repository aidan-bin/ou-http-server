#pragma once

#include "Poller.h"

#include <coroutine>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace ou::http {

// Identifies one request: its connection's fd, and a generation that tells it from later requests on that fd
struct RequestId {
	int fd = -1;
	uint64_t generation = 0;
};

// A suspended request's coroutine to resume on its loop
struct Resumption {
	RequestId request;
	std::coroutine_handle<> coroutine;
};

// Finished pool jobs queued to resume on the event loop that submitted them
class CompletionQueue {
public:
	explicit CompletionQueue(Poller &poller) : poller_(poller) {}

	// Called by a pool thread to queue a finished job; wakes the loop if it was empty
	void push(Resumption resumption) {
		bool wasEmpty = false;
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			wasEmpty = completions_.empty();
			completions_.push_back(resumption);
		}
		if (wasEmpty)
			poller_.wake();
	}

	// Called by the loop to take all finished jobs; returns them in the order they were queued
	[[nodiscard]] std::vector<Resumption> takeAll() {
		const std::lock_guard<std::mutex> lock(mutex_);
		return std::exchange(completions_, {});
	}

private:
	Poller &poller_;
	std::mutex mutex_;
	std::vector<Resumption> completions_;
};

} // namespace ou::http
