#pragma once

#include <map>
#include <sstream>
#include <string>
#include <string_view>

namespace ou::http {

struct Request {
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers;
	std::string body;

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
