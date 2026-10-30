#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ou::http {

// Eager coroutine result; a plain T converts without a frame
template <typename T>
class [[nodiscard]] Task {
public:
	struct promise_type {
		std::variant<std::monostate, T, std::exception_ptr> result;
		std::coroutine_handle<> continuation;

		Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
		std::suspend_never initial_suspend() noexcept { return {}; }

		auto final_suspend() noexcept {
			struct ResumeAwaiting {
				[[nodiscard]] bool await_ready() const noexcept { return false; }
				[[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> finished) const noexcept {
					const std::coroutine_handle<> awaiting = finished.promise().continuation;
					return awaiting ? awaiting : std::noop_coroutine();
				}
				void await_resume() const noexcept {}
			};
			return ResumeAwaiting{};
		}

		void return_value(T value) { result.template emplace<T>(std::move(value)); }
		void unhandled_exception() noexcept { result = std::current_exception(); }
	};

	// Wraps a synchronous result; lets a plain T stand in for Task<T>
	Task(T value) : value_(std::move(value)) {}

	Task(const Task &) = delete;
	Task &operator=(const Task &) = delete;
	Task(Task &&other) noexcept : value_(std::move(other.value_)), handle_(std::exchange(other.handle_, {})) {}
	Task &operator=(Task &&) = delete;
	~Task() {
		if (handle_)
			handle_.destroy();
	}

	// Whether result() can be taken; false while the coroutine is suspended
	[[nodiscard]] bool done() const noexcept { return !handle_ || handle_.done(); }

	// Takes the result; rethrows the coroutine's exception
	[[nodiscard]] T result() {
		if (value_)
			return std::move(*value_);
		if (!handle_)
			throw std::logic_error("Task has no result");

		auto &result = handle_.promise().result;
		if (const auto *error = std::get_if<std::exception_ptr>(&result))
			std::rethrow_exception(*error);
		if (!std::holds_alternative<T>(result))
			throw std::logic_error("Task result taken before it finished");
		return std::move(std::get<T>(result));
	}

	[[nodiscard]] bool await_ready() const noexcept { return done(); }
	void await_suspend(std::coroutine_handle<> awaiting) noexcept { handle_.promise().continuation = awaiting; }
	[[nodiscard]] T await_resume() { return result(); }

private:
	explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

	std::optional<T> value_;
	std::coroutine_handle<promise_type> handle_;
};

namespace detail {

	template <typename T, typename F>
	Task<std::invoke_result_t<F &, T>> thenLater(Task<T> task, F next) {
		co_return next(co_await task);
	}

} // namespace detail

// Passes the task's result to next: right away if it is done, without a coroutine frame, else once it finishes
template <typename T, std::invocable<T> F>
Task<std::invoke_result_t<F &, T>> then(Task<T> task, F next) {
	if (task.done())
		return next(task.result());
	return detail::thenLater(std::move(task), std::move(next));
}

} // namespace ou::http
