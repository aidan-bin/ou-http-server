#pragma once

#include "ou/http/HttpTypes.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace ou::http {

// Accumulates a request across reads
class RequestReader {
public:
	enum class Status : uint8_t { NeedMore, Complete, Rejected };

	static constexpr size_t kChunkBytes = 4096;

	explicit RequestReader(size_t maxRequestBytes) : maxRequestBytes_(maxRequestBytes) {}

	// Appends what was just read and re-evaluates the request's status
	Status consume(std::string_view bytes);

	// The peer stopped sending: a clean close between requests, or a 408 mid-request
	Status endOfInput() noexcept;

	// The status code to answer with; only meaningful after Rejected
	[[nodiscard]] int rejectStatus() const noexcept { return rejectStatus_; }

	// Moves the request out; only valid after Complete.
	// Its views last until the next consume() or next().
	[[nodiscard]] Request take();

	// Drops the finished request and reports on whatever followed it
	Status next();

private:
	Status reject(int status) noexcept;

	Status parseBuffered();
	[[nodiscard]] std::expected<Request, int> parseHead() const;

	size_t maxRequestBytes_;
	std::string raw_;
	size_t scanFrom_ = 0;
	size_t bodyStart_ = std::string::npos;
	size_t requestEnd_ = 0;
	int rejectStatus_ = 400;
	std::optional<Request> request_;
};

} // namespace ou::http
