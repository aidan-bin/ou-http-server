#pragma once

#include <string>
#include <string_view>
#include <map>
#include <sstream>

namespace ou::http {

struct Request {
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers;
	std::string body;

	static Request parse(std::string_view raw);
};

struct Response {
	int statusCode = 200;
	std::string reasonPhrase = "OK";
	std::map<std::string, std::string> headers;
	std::string body;

	std::string serialize() const;
};

} // namespace ou::http
