#include "HttpTypes.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <ostream>
#include <ranges>
#include <sstream>

namespace ou::http {

namespace {

	constexpr char sToLower(char ch) {
		if (ch >= 'A' && ch <= 'Z')
			return static_cast<char>(ch - 'A' + 'a');
		return ch;
	}

	constexpr std::optional<unsigned> sHexValue(char ch) {
		if (ch >= '0' && ch <= '9')
			return static_cast<unsigned>(ch - '0');
		if (ch >= 'a' && ch <= 'f')
			return static_cast<unsigned>(ch - 'a') + 10;
		if (ch >= 'A' && ch <= 'F')
			return static_cast<unsigned>(ch - 'A') + 10;
		return std::nullopt;
	}

} // namespace

std::ostream &operator<<(std::ostream &os, Method method) {
	os << methodToString(method);
	return os;
}

bool CaseInsensitiveLess::operator()(std::string_view lhs, std::string_view rhs) const noexcept {
	return std::ranges::lexicographical_compare(lhs, rhs, {}, sToLower, sToLower);
}

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept { return std::ranges::equal(lhs, rhs, {}, sToLower, sToLower); }

std::string percentDecode(std::string_view input) {
	std::string decoded;
	decoded.reserve(input.size());

	for (size_t i = 0; i < input.size(); ++i) {
		if (input[i] == '+') {
			decoded += ' ';
			continue;
		}

		if (input[i] == '%' && i + 2 < input.size()) {
			const std::optional<unsigned> high = sHexValue(input[i + 1]);
			const std::optional<unsigned> low = sHexValue(input[i + 2]);
			if (high && low) {
				decoded += static_cast<char>((*high * 16) + *low);
				i += 2;
				continue;
			}
		}

		decoded += input[i];
	}

	return decoded;
}

std::map<std::string, std::string> parseQuery(std::string_view query) {
	std::map<std::string, std::string> params;

	for (const auto &pair : std::views::split(query, '&')) {
		const std::string_view field(pair.begin(), pair.end());
		if (field.empty())
			continue;

		const size_t equals = field.find('=');
		if (equals == std::string_view::npos) {
			params[percentDecode(field)] = "";
			continue;
		}

		params[percentDecode(field.substr(0, equals))] = percentDecode(field.substr(equals + 1));
	}

	return params;
}

std::pair<std::string_view, std::string_view> Request::split_at(std::string_view str, char delimiter) {
	const size_t pos = str.find(delimiter);
	if (pos == std::string_view::npos)
		return { str, {} };
	return { str.substr(0, pos), str.substr(pos + 1) };
}

std::string_view Request::trim(std::string_view str) {
	auto isSpace = [](unsigned char ch) {
		return std::isspace(ch) != 0;
	};
	str.remove_prefix(std::ranges::distance(str | std::views::take_while(isSpace)));
	str.remove_suffix(std::ranges::distance(str | std::views::reverse | std::views::take_while(isSpace)));
	return str;
}

std::optional<Request> Request::parse(std::string_view raw) {
	const size_t headerEnd = raw.find("\r\n\r\n");
	const std::string_view headerSection = raw.substr(0, headerEnd);
	const std::string_view bodySection = (headerEnd != std::string_view::npos) ? raw.substr(headerEnd + 4) : std::string_view{};

	auto [requestLine, headerLines] = split_at(headerSection, '\n');
	requestLine = trim(requestLine);
	if (requestLine.empty())
		return std::nullopt;

	auto [methodToken, afterMethod] = split_at(requestLine, ' ');
	auto [target, version] = split_at(afterMethod, ' ');

	const std::optional<Method> method = stringToMethod(trim(methodToken));
	if (!method)
		return std::nullopt;

	target = trim(target);
	if (target.empty() || !trim(version).starts_with("HTTP/"))
		return std::nullopt;

	Request request;
	request.method = *method;

	const size_t queryStart = target.find('?');
	request.path = percentDecode(target.substr(0, queryStart));
	if (queryStart != std::string_view::npos)
		request.query = std::string(target.substr(queryStart + 1));

	if (!request.path.starts_with('/'))
		return std::nullopt;

	while (!headerLines.empty()) {
		auto [line, remaining] = split_at(headerLines, '\n');
		headerLines = remaining;

		line = trim(line);
		if (line.empty())
			continue;

		auto [name, value] = split_at(line, ':');
		name = trim(name);
		if (name.empty())
			return std::nullopt;

		request.headers[std::string(name)] = std::string(trim(value));
	}

	request.body = std::string(bodySection);
	return request;
}

std::optional<std::string_view> Request::header(std::string_view name) const {
	const auto it = headers.find(name);
	if (it == headers.end())
		return std::nullopt;
	return it->second;
}

std::optional<size_t> Request::contentLength() const {
	const std::optional<std::string_view> value = header("Content-Length");
	if (!value)
		return std::nullopt;

	const std::string_view digits = trim(*value);
	if (digits.empty())
		return std::nullopt;

	size_t length = 0;
	for (const char ch : digits) {
		if (ch < '0' || ch > '9')
			return std::nullopt;

		const auto digit = static_cast<size_t>(ch - '0');
		if (length > (std::numeric_limits<size_t>::max() - digit) / 10)
			return std::nullopt;

		length = (length * 10) + digit;
	}

	return length;
}

std::string Response::serialize() const {
	const std::string_view reason = reasonPhrase.empty() ? defaultReasonPhrase(statusCode) : std::string_view(reasonPhrase);

	std::ostringstream oss;
	oss << "HTTP/1.1 " << statusCode << " " << reason << "\r\n";
	oss << "Content-Length: " << body.size() << "\r\n";

	if (!headers.contains("Content-Type"))
		oss << "Content-Type: text/html\r\n";

	for (const auto &[name, value] : headers) {
		if (equalsIgnoreCase(name, "Content-Length"))
			continue;
		oss << name << ": " << value << "\r\n";
	}

	oss << "\r\n" << body;
	return oss.str();
}

Response Response::plainText(int statusCode, std::string body) {
	Response response;
	response.statusCode = statusCode;
	response.headers.emplace("Content-Type", "text/plain");
	response.body = std::move(body);
	return response;
}

Response Response::error(int statusCode) {
	return plainText(statusCode, std::format("{} {}", statusCode, defaultReasonPhrase(statusCode)));
}

} // namespace ou::http
