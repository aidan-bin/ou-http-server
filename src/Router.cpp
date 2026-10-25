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

Router &Router::route(Method method, const std::string &path, AnyHandler handler) {
	pathHandlers_[path][method] = std::move(handler.handler);
	return *this;
}

Router &Router::route(std::initializer_list<Method> methods, const std::string &path, const AnyHandler &handler) {
	for (const Method method : methods) {
		route(method, path, handler.handler);
	}
	return *this;
}

Router &Router::pattern(Method method, const std::string &pattern, AnyHandler handler) {
	const auto existing = std::ranges::find(patternHandlers_, pattern, &PatternRoute::source);
	if (existing != patternHandlers_.end()) {
		existing->handlers.emplace(method, std::move(handler.handler));
		return *this;
	}

	auto [regex, names] = sCompileSegments(pattern);
	PatternRoute route = { .source = pattern, .pattern = std::regex(regex), .names = std::move(names), .handlers = {} };
	route.handlers.emplace(method, std::move(handler.handler));
	patternHandlers_.push_back(std::move(route));
	return *this;
}

Router &Router::pattern(std::initializer_list<Method> methods, const std::string &pattern, const AnyHandler &handler) {
	for (const Method method : methods) {
		this->pattern(method, pattern, handler.handler);
	}
	return *this;
}

Router &Router::layer(std::shared_ptr<Middleware> middleware) {
	layers_.push_back(std::move(middleware));
	return *this;
}

Router &Router::fallback(Handler handler) {
	fallback_ = std::move(handler);
	return *this;
}

Task<Response> Router::handle(Request &request) const {
	Task<Response> handled = dispatch(request);
	if (handled.done())
		return postProcess(request, handled.result());
	return postProcessLater(std::move(handled), request);
}

Task<Response> Router::dispatch(Request &request) const {
	for (const auto &layer : layers_) {
		if (std::optional<Response> response = layer->process(request))
			return std::move(*response);
	}

	std::set<Method> allowed;
	if (const Handler *handler = handlerFor(request, allowed))
		return (*handler)(request);
	if (!allowed.empty())
		return sNotAllowed(allowed);
	return fallback_(request);
}

const Handler *Router::handlerFor(Request &request, std::set<Method> &allowed) const {
	if (const auto path = pathHandlers_.find(request.path); path != pathHandlers_.end()) {
		if (const auto handler = path->second.find(request.method); handler != path->second.end())
			return &handler->second;
		for (const auto &[method, handler] : path->second)
			allowed.insert(method);
	}

	std::smatch match;
	for (const PatternRoute &route : patternHandlers_) {
		if (!std::regex_match(request.path, match, route.pattern))
			continue;

		const auto handler = route.handlers.find(request.method);
		if (handler == route.handlers.end()) {
			for (const auto &[method, unused] : route.handlers)
				allowed.insert(method);
			continue;
		}

		for (size_t i = 0; i < match.size(); ++i)
			request.pathParams.emplace_back(match.position(i), match.length(i));
		request.pathParamNames = route.names;
		return &handler->second;
	}

	return nullptr;
}

Response Router::postProcess(const Request &request, Response response) const {
	for (const auto &layer : layers_) {
		layer->postProcess(request, response);
	}

	if (request.method == Method::HEAD) {
		response.contentLength = response.body.size();
		response.body.clear();
	}

	return response;
}

Task<Response> Router::postProcessLater(Task<Response> handled, const Request &request) const {
	Response response = co_await handled;
	co_return postProcess(request, std::move(response));
}

} // namespace ou::http
