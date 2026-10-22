#pragma once

#include "ou/http/HttpTypes.h"

#include <optional>

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

} // namespace ou::http
