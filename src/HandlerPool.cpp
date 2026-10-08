#include "HandlerPool.h"
#include "Logging.h"

#include <exception>
#include <utility>

namespace ou::http {

HandlerPool::HandlerPool(unsigned threadCount) {
	threads_.reserve(threadCount);
	for (unsigned i = 0; i < threadCount; ++i)
		threads_.emplace_back([this] { work(stop_.get_token()); });
}

HandlerPool::~HandlerPool() { stop(); }

void HandlerPool::submit(std::function<void()> job) {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (stop_.stop_requested())
			return;
		jobs_.push_back(std::move(job));
	}
	ready_.notify_one();
}

void HandlerPool::stop() {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		jobs_.clear();
		stop_.request_stop();
	}
	threads_.clear();
}

void HandlerPool::work(const std::stop_token &stop) {
	while (true) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (!ready_.wait(lock, stop, [this] { return !jobs_.empty(); }))
				return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}

		// Backstop; jobs catch their own exceptions
		try {
			job();
		} catch (const std::exception &e) {
			logError("Unhandled exception in handler pool job: {}", e.what());
		} catch (...) {
			logError("Unhandled exception of unknown type in handler pool job");
		}
	}
}

} // namespace ou::http
