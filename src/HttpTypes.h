#pragma once

#include <format>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <netinet/in.h>

namespace ou::http {

enum class Method { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, CONNECT, TRACE };

Method stringToMethod(std::string_view method);
std::string methodToString(Method method);

std::ostream &operator<<(std::ostream &os, Method method);
std::istream &operator>>(std::istream &is, Method &method);

struct Request {
	Method method;
	std::string path;
	std::map<std::string, std::string> headers;
	std::string body;

	std::optional<sockaddr_in> clientAddr;

	static Request parse(std::string_view raw);

private:
	static std::pair<std::string_view, std::string_view> split_at(std::string_view str, char delimiter);
	static std::string_view trim(std::string_view str);
};

struct Response {
	int statusCode = 200;
	std::string reasonPhrase = "OK";
	std::map<std::string, std::string> headers;
	std::string body;

	std::string serialize() const;
};

} // namespace ou::http

template <> struct std::formatter<ou::http::Method> : std::formatter<std::string> {
	auto format(const ou::http::Method &method, auto &ctx) const {
		return std::formatter<std::string>::format(ou::http::methodToString(method), ctx);
	}
};