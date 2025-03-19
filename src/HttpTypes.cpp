#include "HttpTypes.h"

#include <vector>
#include <cctype>
#include <utility>
#include <algorithm>

namespace ou::http {

Request Request::parse(std::string_view raw) {
	Request req;
	auto trim = [](std::string_view sv) -> std::string_view {
		while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
			sv.remove_prefix(1);
		while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
			sv.remove_suffix(1);
		return sv;
	};

	// Split raw request into lines using "\r\n" as delimiter.
	std::vector<std::string_view> lines;
	size_t start = 0;
	while (start < raw.size()) {
		size_t end = raw.find("\r\n", start);
		if (end == std::string_view::npos) {
			lines.push_back(raw.substr(start));
			break;
		}
		lines.push_back(raw.substr(start, end - start));
		start = end + 2;
	}
	if (lines.empty())
		return req; // malformed

	// Parse request line (first line)
	std::string_view requestLine = lines.front();
	size_t firstSpace = requestLine.find(' ');
	size_t secondSpace = requestLine.find(' ', firstSpace + 1);
	if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos)
		return req; // malformed
	req.method = std::string(trim(requestLine.substr(0, firstSpace)));
	req.path = std::string(trim(requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1)));

	// Parse headers
	bool headerSection = true;
	for (auto it = std::next(lines.begin()); it != lines.end(); ++it) {
		if (headerSection) {
			if (it->empty()) {
				headerSection = false;
				continue;
			}
			auto parseHeader = [&trim](std::string_view line) -> std::pair<std::string_view, std::string_view> {
				size_t colonPos = line.find(':');
				if (colonPos == std::string_view::npos)
					return {line, std::string_view{}};
				return { trim(line.substr(0, colonPos)), trim(line.substr(colonPos + 1)) };
			};
			auto [key, value] = parseHeader(*it);
			if (!key.empty())
				req.headers[std::string(key)] = std::string(value);
		} else {
			if (!req.body.empty())
				req.body.append("\r\n");
			req.body.append(std::string(*it));
		}
	}
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
