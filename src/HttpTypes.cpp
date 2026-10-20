#include "ou/http/HttpTypes.h"

#include <algorithm>
#include <iterator>
#include <ostream>
#include <ranges>

#include <arpa/inet.h>

namespace ou::http {

namespace {

	std::pair<std::string_view, std::string_view> sSplitAt(std::string_view str, char delimiter) {
		const size_t pos = str.find(delimiter);
		if (pos == std::string_view::npos)
			return { str, {} };
		return { str.substr(0, pos), str.substr(pos + 1) };
	}

	constexpr char sToLower(char ch) {
		if (ch >= 'A' && ch <= 'Z')
			return static_cast<char>(ch - 'A' + 'a');
		return ch;
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

std::ostream &operator<<(std::ostream &os, Method method) {
	os << methodToString(method);
	return os;
}

bool CaseInsensitiveLess::operator()(std::string_view lhs, std::string_view rhs) const noexcept {
	return std::ranges::lexicographical_compare(lhs, rhs, {}, sToLower, sToLower);
}

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept { return std::ranges::equal(lhs, rhs, {}, sToLower, sToLower); }

std::string percentDecode(std::string_view input, bool plusAsSpace) {
	std::string decoded;
	decoded.reserve(input.size());

	for (size_t i = 0; i < input.size(); ++i) {
		if (plusAsSpace && input[i] == '+') {
			decoded += ' ';
			continue;
		}

		if (input[i] == '%' && i + 2 < input.size()) {
			if (const std::optional<unsigned char> byte = parseNumber<unsigned char>(input.substr(i + 1, 2), 16)) {
				decoded += static_cast<char>(*byte);
				i += 2;
				continue;
			}
		}

		decoded += input[i];
	}

	return decoded;
}

std::string percentEncode(std::string_view input) {
	constexpr std::string_view kUnreserved = "-_.~/";

	std::string encoded;
	encoded.reserve(input.size());

	for (const unsigned char ch : input) {
		const bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
														|| kUnreserved.find(static_cast<char>(ch)) != std::string_view::npos;
		if (unreserved) {
			encoded += static_cast<char>(ch);
			continue;
		}
		encoded += std::format("%{:02X}", static_cast<unsigned>(ch));
	}

	return encoded;
}

std::string addressToString(const sockaddr_in &address) {
	std::array<char, INET_ADDRSTRLEN> buffer{};
	if (inet_ntop(AF_INET, &address.sin_addr, buffer.data(), buffer.size()) == nullptr)
		return "unknown";
	return { buffer.data() };
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

std::string_view trim(std::string_view str) {
	constexpr std::string_view kWhitespace = " \t\n\v\f\r";
	const size_t first = str.find_first_not_of(kWhitespace);
	if (first == std::string_view::npos)
		return {};
	return str.substr(first, str.find_last_not_of(kWhitespace) - first + 1);
}

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
	return header("Content-Length").and_then([](std::string_view value) { return parseNumber<size_t>(trim(value)); });
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

std::string Response::serialize() const {
	const std::string_view reason = reasonPhrase.empty() ? defaultReasonPhrase(statusCode) : std::string_view(reasonPhrase);

	constexpr size_t kHeadEstimate = 256;
	std::string serialized;
	serialized.reserve(kHeadEstimate + body.size());
	auto out = std::back_inserter(serialized);

	std::format_to(out, "HTTP/1.1 {} {}\r\nContent-Length: {}\r\nConnection: {}\r\n", statusCode, reason, body.size(),
								 keepAlive ? "keep-alive" : "close");

	if (!headers.contains("Content-Type"))
		serialized += "Content-Type: text/html\r\n";

	for (const auto &[name, value] : headers) {
		if (equalsIgnoreCase(name, "Content-Length") || equalsIgnoreCase(name, "Connection"))
			continue;
		std::format_to(out, "{}: {}\r\n", name, value);
	}

	serialized += "\r\n";
	serialized += body;
	return serialized;
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
