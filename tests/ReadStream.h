#pragma once

#include "RequestReader.h"
#include "ou/http/HttpTypes.h"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ou::test {

using http::Request;
using http::RequestReader;

inline constexpr size_t kReadStreamMaxBytes = 1024;

// Feeds the stream to a RequestReader in chunkSize reads; one event per request or rejection
inline std::vector<std::string> readStream(std::string_view stream, size_t chunkSize) {
	RequestReader reader(kReadStreamMaxBytes);
	std::vector<std::string> events;

	auto drain = [&](RequestReader::Status status) {
		while (status == RequestReader::Status::Complete) {
			const Request request = reader.take();
			events.push_back(
					std::format("{} {} {} {} {}", request.method, request.path, request.query, request.header("Host").value_or(""), request.body));
			status = reader.next();
		}
		if (status == RequestReader::Status::Rejected)
			events.push_back(std::format("reject {}", reader.rejectStatus()));
		return status != RequestReader::Status::Rejected;
	};

	for (size_t offset = 0; offset < stream.size(); offset += chunkSize) {
		if (!drain(reader.consume(stream.substr(offset, chunkSize))))
			return events;
	}
	drain(reader.endOfInput());
	return events;
}

} // namespace ou::test
