#include "ou/http/Request.h"
#include "ou/http/Strings.h"

#include <algorithm>
#include <ranges>

namespace ou::http {

namespace {

	std::pair<std::string_view, std::string_view> sSplitAt(std::string_view str, char delimiter) {
		const size_t pos = str.find(delimiter);
		if (pos == std::string_view::npos)
			return { str, {} };
		return { str.substr(0, pos), str.substr(pos + 1) };
	}

	// "HTTP/" DIGIT "." DIGIT
	bool sIsHttpVersion(std::string_view version) {
		const auto isDigit = [](char ch) {
			return ch >= '0' && ch <= '9';
		};
		return version.size() == 8 && version.starts_with("HTTP/") && isDigit(version[5]) && version[6] == '.' && isDigit(version[7]);
	}

	bool sHasToken(std::string_view fieldValue, std::string_view token) {
		const auto listed = [token](const auto &part) {
			return equalsIgnoreCase(trim(std::string_view(part.begin(), part.end())), token);
		};
		return std::ranges::any_of(std::views::split(fieldValue, ','), listed);
	}

} // namespace

std::expected<Request, int> Request::parse(std::string_view head) {
	auto [requestLine, headerLines] = sSplitAt(head, '\n');
	requestLine = trim(requestLine);
	if (requestLine.empty())
		return std::unexpected(400);

	auto [methodToken, afterMethod] = sSplitAt(requestLine, ' ');
	auto [target, version] = sSplitAt(afterMethod, ' ');

	const std::optional<Method> method = stringToMethod(trim(methodToken));
	if (!method)
		return std::unexpected(400);

	target = trim(target);
	const std::string_view httpVersion = trim(version);
	if (target.empty() || !sIsHttpVersion(httpVersion))
		return std::unexpected(400);
	if (httpVersion != "HTTP/1.0" && httpVersion != "HTTP/1.1")
		return std::unexpected(505);

	Request request;
	request.method = *method;
	request.http10 = httpVersion == "HTTP/1.0";

	const size_t queryStart = target.find('?');
	request.path = percentDecode(target.substr(0, queryStart), false);
	if (queryStart != std::string_view::npos)
		request.query = target.substr(queryStart + 1);

	if (!request.path.starts_with('/'))
		return std::unexpected(400);

	request.headers.reserve(std::ranges::count(headerLines, '\n') + 1);
	while (!headerLines.empty()) {
		auto [line, remaining] = sSplitAt(headerLines, '\n');
		headerLines = remaining;

		line = trim(line);
		if (line.empty())
			continue;

		const size_t colon = line.find(':');
		if (colon == 0 || colon == std::string_view::npos)
			return std::unexpected(400);

		const std::string_view name = line.substr(0, colon);
		if (name.back() == ' ' || name.back() == '\t')
			return std::unexpected(400);

		const std::string_view value = trim(line.substr(colon + 1));
		if (equalsIgnoreCase(name, "Content-Length") && request.header(name).value_or(value) != value)
			return std::unexpected(400);
		request.headers.emplace_back(name, value);
	}

	// HTTP/1.1 requires exactly one Host
	if (!request.http10
	    && std::ranges::count_if(request.headers, [](const auto &field) { return equalsIgnoreCase(field.first, "Host"); }) != 1)
		return std::unexpected(400);

	return request;
}

std::optional<std::string_view> Request::header(std::string_view name) const {
	for (const auto &[field, value] : headers | std::views::reverse) {
		if (equalsIgnoreCase(field, name))
			return value;
	}
	return std::nullopt;
}

std::optional<size_t> Request::contentLength() const {
	return header("Content-Length").and_then([](std::string_view value) { return parseNumber<size_t>(value); });
}

std::optional<std::string_view> Request::pathParam(size_t index) const {
	if (index >= pathParams.size())
		return std::nullopt;
	const auto [offset, length] = pathParams[index];
	return std::string_view(path).substr(offset, length);
}

std::optional<std::string_view> Request::pathParam(std::string_view name) const {
	const auto found = std::ranges::find(pathParamNames, name);
	if (found == pathParamNames.end())
		return std::nullopt;
	return pathParam(static_cast<size_t>(found - pathParamNames.begin()) + 1);
}

bool Request::wantsKeepAlive() const {
	const std::optional<std::string_view> connection = header("Connection");

	if (http10)
		return connection && sHasToken(*connection, "keep-alive");

	return !connection || !sHasToken(*connection, "close");
}

} // namespace ou::http
