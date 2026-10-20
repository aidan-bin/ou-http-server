#pragma once

#include "ou/http/Extractors.h"
#include "ou/http/HttpTypes.h"
#include "ou/http/Middleware.h"
#include "ou/http/Task.h"

#include <concepts>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace ou::http {

// Function or coroutine that handles a request and returns a response
using Handler = std::function<Task<Response>(const Request &)>;

// A Handler, or a lambda taking Extractors (wrapped by extracting())
struct AnyHandler {
	template <typename F>
	AnyHandler(F handler) { // NOLINT(*-explicit-*) converts at the call site by design
		if constexpr (std::convertible_to<F, Handler>)
			this->handler = std::move(handler);
		else
			this->handler = extracting(std::move(handler));
	}

	Handler handler;
};

// Maps requests to handlers. Built up front, then handed to a Server, which does not change it.
class Router {
public:
	// Re-registering an exact path replaces its handler
	Router &route(Method method, const std::string &path, AnyHandler handler);
	Router &route(std::initializer_list<Method> methods, const std::string &path, const AnyHandler &handler);

	// Patterns accumulate and the first match wins. A pattern with {name} segments matches its other
	// text literally; one without is a regex.
	Router &pattern(Method method, const std::string &pattern, AnyHandler handler);
	Router &pattern(std::initializer_list<Method> methods, const std::string &pattern, const AnyHandler &handler);

	// Runs for every request, in the order added
	Router &layer(std::shared_ptr<Middleware> middleware);

	// Answers requests that no route matches (404 by default)
	Router &fallback(Handler handler);
	[[nodiscard]] bool hasFallback() const noexcept { return fallback_.has_value(); }

	// Layers' process(), then the exact path, then patterns, then the fallback, then every layer's
	// postProcess(). May finish later if the handler suspends.
	[[nodiscard]] Task<Response> handle(Request &request) const;

	// Runs every layer's postProcess() and drops the body for HEAD; for a response with no routed request
	[[nodiscard]] Response postProcess(const Request &request, Response response) const;

private:
	struct PatternRoute {
		Method method = Method::GET;
		std::regex pattern;
		std::vector<std::string> names;
		Handler handler;
	};

	// Exact path, then pattern, then the fallback; stores pattern captures in request
	[[nodiscard]] const Handler &handlerFor(Request &request) const;
	[[nodiscard]] Task<Response> dispatch(Request &request) const;
	[[nodiscard]] Task<Response> postProcessLater(Task<Response> handled, const Request &request) const;

	// Exact paths are looked up; patterns are tried in registration order
	std::map<std::pair<Method, std::string>, Handler> pathHandlers_;
	std::vector<PatternRoute> patternHandlers_;
	std::optional<Handler> fallback_;
	std::vector<std::shared_ptr<Middleware>> layers_;
};

} // namespace ou::http
