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
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ou::http {

// Function or coroutine that handles a request and returns a response
using Handler = std::function<Task<Response>(const Request &)>;

// A Handler, or a lambda taking Extractors and returning anything IntoResponse (wrapped by extracting())
struct AnyHandler {
	template <typename F>
	AnyHandler(F handler) {
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

	// Answers requests that no route matches (404 by default), e.g., with StaticFiles
	Router &fallback(Handler handler);

	// Layers' process(), then the exact path, then patterns, then 405 if the path has routes for other methods,
	// then the fallback, then every layer's postProcess(). May finish later if the handler suspends.
	[[nodiscard]] Task<Response> handle(Request &request) const;

	// Runs every layer's postProcess() and drops the body for HEAD; for a response with no routed request
	[[nodiscard]] Response postProcess(const Request &request, Response response) const;

private:
	struct PatternRoute {
		std::string source;
		std::regex pattern;
		std::vector<std::string> names;
		std::map<Method, Handler> handlers;
	};

	// Exact path, then pattern; null if none. Stores pattern captures in request, and adds to `allowed` the methods
	// that the path is routed for but the request's is not
	[[nodiscard]] const Handler *handlerFor(Request &request, std::set<Method> &allowed) const;
	[[nodiscard]] Task<Response> dispatch(Request &request) const;
	[[nodiscard]] Task<Response> postProcessLater(Task<Response> handled, const Request &request) const;

	// Exact paths are looked up; patterns are tried in registration order
	std::map<std::string, std::map<Method, Handler>> pathHandlers_;
	std::vector<PatternRoute> patternHandlers_;
	Handler fallback_ = [](const Request &) {
		return Response::error(404);
	};
	std::vector<std::shared_ptr<Middleware>> layers_;
};

} // namespace ou::http
