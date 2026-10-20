#include "ou/http/Router.h"

#include <optional>
#include <string_view>

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

} // namespace

namespace ou::http {

Router &Router::route(Method method, const std::string &path, AnyHandler handler) {
	pathHandlers_[{ method, path }] = std::move(handler.handler);
	return *this;
}

Router &Router::route(std::initializer_list<Method> methods, const std::string &path, const AnyHandler &handler) {
	for (const Method method : methods) {
		route(method, path, handler.handler);
	}
	return *this;
}

Router &Router::pattern(Method method, const std::string &pattern, AnyHandler handler) {
	auto [regex, names] = sCompileSegments(pattern);
	patternHandlers_.push_back(
			{ .method = method, .pattern = std::regex(regex), .names = std::move(names), .handler = std::move(handler.handler) });
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

	return handlerFor(request)(request);
}

const Handler &Router::handlerFor(Request &request) const {
	if (const auto path = pathHandlers_.find({ request.method, request.path }); path != pathHandlers_.end())
		return path->second;

	std::smatch match;
	for (const PatternRoute &route : patternHandlers_) {
		if (route.method != request.method || !std::regex_match(request.path, match, route.pattern))
			continue;

		for (size_t i = 0; i < match.size(); ++i)
			request.pathParams.emplace_back(match.position(i), match.length(i));
		request.pathParamNames = route.names;
		return route.handler;
	}

	return fallback_;
}

Response Router::postProcess(const Request &request, Response response) const {
	for (const auto &layer : layers_) {
		layer->postProcess(request, response);
	}

	if (request.method == Method::HEAD)
		response.body.clear();

	return response;
}

Task<Response> Router::postProcessLater(Task<Response> handled, const Request &request) const {
	Response response = co_await handled;
	co_return postProcess(request, std::move(response));
}

} // namespace ou::http
