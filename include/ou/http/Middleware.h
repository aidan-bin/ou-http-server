#pragma once

#include "ou/http/Request.h"
#include "ou/http/Response.h"

#include <concepts>
#include <memory>
#include <optional>
#include <utility>

namespace ou::http {

// Wraps routing: before() sees each request first, after() sees each final response
class Middleware {
public:
	Middleware() = default;
	Middleware(const Middleware &) = delete;
	Middleware &operator=(const Middleware &) = delete;
	Middleware(Middleware &&) = delete;
	Middleware &operator=(Middleware &&) = delete;
	virtual ~Middleware() = default;

	// Runs before routing. Return a response to answer with it and skip routing; may modify `request`
	virtual std::optional<Response> before(Request & /*request*/) { return std::nullopt; }

	// Runs after every request (including short-circuited and rejected ones); may modify the final `response`
	virtual void after(const Request & /*request*/, Response & /*response*/) {}
};

// A middleware whose before() is `hook`
template <std::invocable<Request &> F>
std::shared_ptr<Middleware> before(F hook) {
	class Before final : public Middleware {
	public:
		explicit Before(F hook) : hook_(std::move(hook)) {}
		std::optional<Response> before(Request &request) final { return hook_(request); }

	private:
		F hook_;
	};
	return std::make_shared<Before>(std::move(hook));
}

// A middleware whose after() is `hook`
template <std::invocable<const Request &, Response &> F>
std::shared_ptr<Middleware> after(F hook) {
	class After final : public Middleware {
	public:
		explicit After(F hook) : hook_(std::move(hook)) {}
		void after(const Request &request, Response &response) final { hook_(request, response); }

	private:
		F hook_;
	};
	return std::make_shared<After>(std::move(hook));
}

} // namespace ou::http
