#include "ou/http/Router.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace ou::http {

namespace {

	// Regex and group names for a {name} pattern; any other pattern is already a regex
	std::pair<std::string, std::vector<std::string>> sCompileSegments(const std::string &pattern) {
		const std::regex segment(R"(\{([A-Za-z_]\w*)\})");
		std::string regex;
		std::vector<std::string> names;
		const auto appendLiteral = [&regex](std::string_view text) {
			for (const char c : text) {
				if (std::string_view(R"(\^$.|?*+()[]{})").contains(c))
					regex += '\\';
				regex += c;
			}
		};

		std::string_view rest = pattern;
		for (auto it = std::sregex_iterator(pattern.begin(), pattern.end(), segment); it != std::sregex_iterator(); ++it) {
			appendLiteral(it->prefix().str());
			regex += "([^/]+)";
			names.push_back((*it)[1].str());
			rest = std::string_view(pattern).substr(it->position() + it->length());
		}

		if (names.empty())
			return { pattern, {} };
		appendLiteral(rest);
		return { std::move(regex), std::move(names) };
	}

	Response sNotAllowed(const std::set<Method> &allowed) {
		std::string allow;
		for (const Method method : allowed)
			allow += (allow.empty() ? "" : ", ") + std::string(methodToString(method));
		return Response::notAllowed(std::move(allow));
	}

} // namespace

void Router::addRoute(const std::string &path, MethodHandler handler) {
	if (path.contains('{'))
		addPattern(path, std::move(handler));
	else
		exactRoutes_[path][handler.method] = std::move(handler.handler);
}

void Router::addPattern(const std::string &pattern, MethodHandler handler) {
	const auto existing = std::ranges::find(patternRoutes_, pattern, &PatternRoute::source);
	if (existing != patternRoutes_.end()) {
		existing->handlers.emplace(handler.method, std::move(handler.handler));
		return;
	}

	auto [regex, names] = sCompileSegments(pattern);
	PatternRoute route = { .source = pattern, .regex = std::regex(regex), .names = std::move(names), .handlers = {} };
	route.handlers.emplace(handler.method, std::move(handler.handler));
	patternRoutes_.push_back(std::move(route));
}

Router &Router::use(std::shared_ptr<Middleware> added) {
	middleware_.push_back(std::move(added));
	return *this;
}

Router &Router::fallback(Handler handler) {
	fallback_ = std::move(handler);
	return *this;
}

Task<Response> Router::handle(Request &request) const {
	return then(answer(request), [this, &request](Response response) { return finish(request, std::move(response)); });
}

Task<Response> Router::answer(Request &request) const {
	for (const auto &middleware : middleware_) {
		if (std::optional<Response> response = middleware->before(request))
			return std::move(*response);
	}

	std::set<Method> allowed;
	if (const Handler *handler = match(request, allowed))
		return (*handler)(request);
	if (!allowed.empty())
		return sNotAllowed(allowed);
	return fallback_(request);
}

const Handler *Router::match(Request &request, std::set<Method> &allowed) const {
	if (const auto exact = exactRoutes_.find(request.path); exact != exactRoutes_.end()) {
		if (const auto handler = exact->second.find(request.method); handler != exact->second.end())
			return &handler->second;
		for (const auto &[method, unused] : exact->second)
			allowed.insert(method);
	}

	std::smatch captures;
	for (const PatternRoute &route : patternRoutes_) {
		if (!std::regex_match(request.path, captures, route.regex))
			continue;

		const auto handler = route.handlers.find(request.method);
		if (handler == route.handlers.end()) {
			for (const auto &[method, unused] : route.handlers)
				allowed.insert(method);
			continue;
		}

		for (size_t i = 0; i < captures.size(); ++i)
			request.pathParams.emplace_back(captures.position(i), captures.length(i));
		request.pathParamNames = route.names;
		return &handler->second;
	}

	return nullptr;
}

Response Router::finish(const Request &request, Response response) const {
	for (const auto &middleware : middleware_) {
		middleware->after(request, response);
	}

	if (request.method == Method::HEAD) {
		response.contentLength = response.body.size();
		response.body.clear();
	}

	return response;
}

} // namespace ou::http
