#include "ou/http/Response.h"

#include <format>
#include <iterator>

namespace ou::http {

std::string Response::serialize() const {
	const std::string_view reason = reasonPhrase.empty() ? defaultReasonPhrase(statusCode) : std::string_view(reasonPhrase);

	constexpr size_t kHeadEstimate = 256;
	std::string serialized;
	serialized.reserve(kHeadEstimate + body.size());
	auto out = std::back_inserter(serialized);

	std::format_to(out, "HTTP/1.1 {} {}\r\nContent-Length: {}\r\nConnection: {}\r\n", statusCode, reason, contentLength.value_or(body.size()),
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

Response Response::notAllowed(std::string allow) {
	Response response = error(405);
	response.headers.emplace("Allow", std::move(allow));
	return response;
}

} // namespace ou::http
