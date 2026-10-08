#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace ou::http {

// Thread pool to run handlers on (so they don't block the event loop)
class HandlerPool {
public:
	explicit HandlerPool(unsigned threadCount);
	HandlerPool(const HandlerPool &) = delete;
	HandlerPool &operator=(const HandlerPool &) = delete;
	HandlerPool(HandlerPool &&) = delete;
	HandlerPool &operator=(HandlerPool &&) = delete;
	~HandlerPool();

	// Queues job for a pool thread; dropped after stop()
	void submit(std::function<void()> job);

	// Discards queued jobs, lets running ones finish, and joins; idempotent
	void stop();

private:
	void work(const std::stop_token &stop);

	std::stop_source stop_;
	std::mutex mutex_;
	std::condition_variable_any ready_;
	std::deque<std::function<void()>> jobs_;
	std::vector<std::jthread> threads_;
};

} // namespace ou::http
