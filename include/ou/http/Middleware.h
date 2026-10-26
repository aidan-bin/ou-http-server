#pragma once

#include "ou/http/HttpTypes.h"

#include <concepts>
#include <memory>
#include <optional>
#include <utility>

namespace ou::http {

class Middleware {
public:
	Middleware() = default;
	Middleware(const Middleware &) = delete;
	Middleware &operator=(const Middleware &) = delete;
	Middleware(Middleware &&) = delete;
	Middleware &operator=(Middleware &&) = delete;
	virtual ~Middleware() = default;

	// Runs before routing. Return a response to answer with it and skip routing; may modify `request`
	virtual std::optional<Response> process(Request & /*request*/) { return std::nullopt; }

	// Runs after every request (including short-circuited and rejected ones); may modify the final `response`
	virtual void postProcess(const Request & /*request*/, Response & /*response*/) {}
};

// A layer whose process() is `process`
template <std::invocable<Request &> F>
std::shared_ptr<Middleware> before(F process) {
	class Before final : public Middleware {
	public:
		explicit Before(F process) : process_(std::move(process)) {}
		std::optional<Response> process(Request &request) final { return process_(request); }

	private:
		F process_;
	};
	return std::make_shared<Before>(std::move(process));
}

// A layer whose postProcess() is `postProcess`
template <std::invocable<const Request &, Response &> F>
std::shared_ptr<Middleware> after(F postProcess) {
	class After final : public Middleware {
	public:
		explicit After(F postProcess) : postProcess_(std::move(postProcess)) {}
		void postProcess(const Request &request, Response &response) final { postProcess_(request, response); }

	private:
		F postProcess_;
	};
	return std::make_shared<After>(std::move(postProcess));
}

} // namespace ou::http
