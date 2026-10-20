#include "ou/http/Awaitables.h"
#include "EventLoop.h"

#include <chrono>
#include <coroutine>
#include <functional>
#include <utility>

namespace ou::http::detail {

bool onEventLoop() noexcept { return EventLoop::current() != nullptr; }

bool canOffload() noexcept {
	const EventLoop *loop = EventLoop::current();
	return loop != nullptr && loop->hasPool();
}

void resumeAt(std::chrono::steady_clock::time_point deadline, std::coroutine_handle<> coroutine) {
	EventLoop::current()->resumeAt(deadline, coroutine);
}

void resumeAfter(std::function<void()> work, std::coroutine_handle<> coroutine) {
	EventLoop::current()->resumeAfter(std::move(work), coroutine);
}

} // namespace ou::http::detail
