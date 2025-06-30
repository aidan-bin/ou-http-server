#include "HttpTypes.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace ou::http {

inline Method stringToMethod(std::string_view method) {
	if (method == "GET")
		return Method::GET;
	if (method == "POST")
		return Method::POST;
	if (method == "PUT")
		return Method::PUT;
	if (method == "DELETE")
		return Method::DELETE;
	if (method == "PATCH")
		return Method::PATCH;
	if (method == "HEAD")
		return Method::HEAD;
	if (method == "OPTIONS")
		return Method::OPTIONS;
	if (method == "CONNECT")
		return Method::CONNECT;
	if (method == "TRACE")
		return Method::TRACE;

	throw std::invalid_argument("Invalid HTTP method: " + std::string(method));
}

inline std::string methodToString(Method method) {
	switch (method) {
	case Method::GET:
		return "GET";
	case Method::POST:
		return "POST";
	case Method::PUT:
		return "PUT";
	case Method::DELETE:
		return "DELETE";
	case Method::PATCH:
		return "PATCH";
	case Method::HEAD:
		return "HEAD";
	case Method::OPTIONS:
		return "OPTIONS";
	case Method::CONNECT:
		return "CONNECT";
	case Method::TRACE:
		return "TRACE";
	default:
		return "UNKNOWN";
	}
}

std::ostream &operator<<(std::ostream &os, Method method) {
	os << methodToString(method);
	return os;
}

std::istream &operator>>(std::istream &is, Method &method) {
	std::string token;
	is >> token;
	method = stringToMethod(token);
	return is;
}

std::pair<std::string_view, std::string_view> Request::split_at(std::string_view str, char delimiter) {
	size_t pos = str.find(delimiter);
	if (pos == std::string_view::npos)
		return { str, {} };
	return { str.substr(0, pos), str.substr(pos + 1) };
}

std::string_view Request::trim(std::string_view str) {
	auto isSpace = [](unsigned char ch) {
		return std::isspace(ch);
	};
	str.remove_prefix(std::ranges::distance(str | std::views::take_while(isSpace)));
	str.remove_suffix(std::ranges::distance(str | std::views::reverse | std::views::take_while(isSpace)));
	return str;
}

Request Request::parse(std::string_view raw) {
	Request req;
	size_t pos = raw.find("\r\n\r\n");
	std::string_view headerSection = raw.substr(0, pos);
	std::string_view bodySection = (pos != std::string_view::npos) ? raw.substr(pos + 4) : "";

	std::string headerStr(headerSection); // Ensure persistent storage
	std::istringstream headerStream(headerStr);
	std::string line;

	if (!std::getline(headerStream, line) || line.empty()) {
		throw std::runtime_error("Invalid request: missing request line");
	}

	std::istringstream requestLine(line);
	if (!(requestLine >> req.method >> req.path)) {
		throw std::runtime_error("Invalid request line format");
	}

	while (std::getline(headerStream, line) && !line.empty()) {
		auto [key, value] = split_at(line, ':');
		req.headers[std::string(trim(key))] = std::string(trim(value));
	}

	req.body = std::string(bodySection);
	return req;
}

std::string Response::serialize() const {
	std::ostringstream oss;
	oss << "HTTP/1.1 " << statusCode << " " << reasonPhrase << "\r\n";
	oss << "Content-Length: " << body.size() << "\r\n";
	if (headers.find("Content-Type") == headers.end())
		oss << "Content-Type: text/html\r\n";
	for (const auto &header : headers)
		oss << header.first << ": " << header.second << "\r\n";
	oss << "\r\n";
	oss << body;
	return oss.str();
}

} // namespace ou::http
