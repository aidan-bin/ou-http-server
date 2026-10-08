#pragma once

#include "EventLoop.h"

#include <chrono>
#include <coroutine>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace ou::http {

// Suspends a handler until a deadline on its event loop (sleeps the thread if not on an event loop)
class SleepAwaiter {
public:
	explicit SleepAwaiter(std::chrono::milliseconds duration) : deadline_(std::chrono::steady_clock::now() + duration) {}

	[[nodiscard]] bool await_ready() const {
		if (EventLoop::current() != nullptr)
			return deadline_ <= std::chrono::steady_clock::now();

		std::this_thread::sleep_until(deadline_);
		return true;
	}

	void await_suspend(std::coroutine_handle<> coroutine) const { EventLoop::current()->resumeAt(deadline_, coroutine); }
	void await_resume() const noexcept {}

private:
	std::chrono::steady_clock::time_point deadline_;
};

// Runs work on the handler pool and resumes the handler with its result (runs inline if not on an event loop)
template <typename R>
class OffloadAwaiter {
public:
	explicit OffloadAwaiter(std::function<R()> work) : work_(std::move(work)) {}

	[[nodiscard]] bool await_ready() {
		if (const EventLoop *loop = EventLoop::current(); loop != nullptr && loop->hasPool())
			return false;

		run(work_, *result_);
		return true;
	}

	void await_suspend(std::coroutine_handle<> coroutine) {
		EventLoop::current()->resumeAfter([work = std::move(work_), result = result_] { run(work, *result); }, coroutine);
	}

	[[nodiscard]] R await_resume() {
		if (!*result_)
			std::rethrow_exception(result_->error());
		return std::move(**result_);
	}

private:
	static void run(const std::function<R()> &work, std::expected<R, std::exception_ptr> &result) {
		try {
			result = work();
		} catch (...) {
			result = std::unexpected(std::current_exception());
		}
	}

	std::function<R()> work_;
	// Shared with the job; outlives the awaiter if the request ends first
	std::shared_ptr<std::expected<R, std::exception_ptr>> result_ = std::make_shared<std::expected<R, std::exception_ptr>>();
};

// Suspends the handler for duration
[[nodiscard]] inline SleepAwaiter sleepFor(std::chrono::milliseconds duration) { return SleepAwaiter(duration); }

// Runs work off the event loop; returns what work returns
template <typename F>
[[nodiscard]] auto offload(F work) {
	return OffloadAwaiter<std::invoke_result_t<F &>>(std::move(work));
}

} // namespace ou::http
