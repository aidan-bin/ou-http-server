#pragma once

#include "ou/http/Extractors.h"
#include "ou/http/HttpTypes.h"
#include "ou/http/Middleware.h"
#include "ou/http/Task.h"

#include <concepts>
#include <functional>
#include <map>
#include <memory>
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

// A handler for one method of a route, made by get(), put() and so on
struct MethodHandler {
	Method method;
	Handler handler;
};

inline MethodHandler get(AnyHandler handler) { return { Method::GET, std::move(handler.handler) }; }
inline MethodHandler head(AnyHandler handler) { return { Method::HEAD, std::move(handler.handler) }; }
inline MethodHandler post(AnyHandler handler) { return { Method::POST, std::move(handler.handler) }; }
inline MethodHandler put(AnyHandler handler) { return { Method::PUT, std::move(handler.handler) }; }
inline MethodHandler patch(AnyHandler handler) { return { Method::PATCH, std::move(handler.handler) }; }
inline MethodHandler del(AnyHandler handler) { return { Method::DELETE, std::move(handler.handler) }; } // delete is a keyword

// Maps requests to handlers. Built up front, then handed to a Server, which does not change it.
class Router {
public:
	// An exact path, or a pattern if it has {name} segments. Re-registering a method on an exact path replaces its handler.
	Router &route(const std::string &path, std::same_as<MethodHandler> auto... handlers) {
		(addRoute(path, std::move(handlers)), ...);
		return *this;
	}

	// A regex, which may also use {name} segments. Patterns are tried in registration order and the first match wins.
	Router &pattern(const std::string &pattern, std::same_as<MethodHandler> auto... handlers) {
		(addPattern(pattern, std::move(handlers)), ...);
		return *this;
	}

	// Runs for every request, in the order added
	Router &use(std::shared_ptr<Middleware> added);

	// Answers requests that no route matches (404 by default), e.g., with StaticFiles
	Router &fallback(Handler handler);

	// Each middleware's before(), then the exact path, then patterns, then 405 if the path has routes for other methods,
	// then the fallback, then every middleware's after(). May finish later if the handler suspends.
	[[nodiscard]] Task<Response> handle(Request &request) const;

	// Runs every middleware's after() and drops the body for HEAD; also for a response to a request that never parsed
	[[nodiscard]] Response finish(const Request &request, Response response) const;

private:
	struct PatternRoute {
		std::string source;
		std::regex pattern;
		std::vector<std::string> names;
		std::map<Method, Handler> handlers;
	};

	void addRoute(const std::string &path, MethodHandler handler);
	void addPattern(const std::string &pattern, MethodHandler handler);

	// Exact path, then pattern; null if none. Stores pattern captures in request, and adds to `allowed` the methods
	// that the path is routed for but the request's is not
	[[nodiscard]] const Handler *handlerFor(Request &request, std::set<Method> &allowed) const;
	[[nodiscard]] Task<Response> dispatch(Request &request) const;

	// Exact paths are looked up; patterns are tried in registration order
	std::map<std::string, std::map<Method, Handler>> pathHandlers_;
	std::vector<PatternRoute> patternHandlers_;
	Handler fallback_ = [](const Request &) {
		return Response::error(404);
	};
	std::vector<std::shared_ptr<Middleware>> middleware_;
};

} // namespace ou::http
