#include "RequestReader.h"

#include <algorithm>
#include <expected>
#include <utility>

namespace ou::http {

namespace {

	constexpr std::string_view kHeaderTerminator = "\r\n\r\n";

} // namespace

RequestReader::Status RequestReader::reject(int status) noexcept {
	rejectStatus_ = status;
	return Status::Rejected;
}

RequestReader::Status RequestReader::consume(std::string_view bytes) {
	raw_.append(bytes);
	return parseBuffered();
}

RequestReader::Status RequestReader::abandon() noexcept { return raw_.empty() ? Status::NeedMore : reject(408); }

RequestReader::Status RequestReader::reset() {
	// Keep any pipelined bytes after the finished request and parse them now
	raw_.erase(0, std::min(totalSize_, raw_.size()));
	bodyStart_ = std::string::npos;
	totalSize_ = 0;
	request_.reset();

	return parseBuffered();
}

RequestReader::Status RequestReader::parseBuffered() {
	if (raw_.empty())
		return Status::NeedMore;

	if (raw_.size() > maxRequestBytes_)
		return reject(413);

	if (bodyStart_ == std::string::npos) {
		const size_t headerEnd = raw_.find(kHeaderTerminator);
		if (headerEnd == std::string::npos)
			return Status::NeedMore;
		bodyStart_ = headerEnd + kHeaderTerminator.size();
	}

	if (!request_) {
		std::expected<Request, int> parsed = Request::parse(raw_);
		if (!parsed)
			return reject(parsed.error());
		request_ = std::move(*parsed);

		// Chunked bodies aren't supported
		if (request_->header("Transfer-Encoding"))
			return reject(501);

		const std::optional<size_t> contentLength = request_->contentLength();
		if (!contentLength && request_->header("Content-Length"))
			return reject(400);

		if (contentLength.value_or(0) > maxRequestBytes_ - bodyStart_)
			return reject(413);
		totalSize_ = bodyStart_ + contentLength.value_or(0);
	}

	return raw_.size() < totalSize_ ? Status::NeedMore : Status::Complete;
}

Request RequestReader::take() {
	if (!request_)
		return {};

	// Whatever follows the body (given by Content-Length) stays in raw_ for reset()
	request_->body.assign(raw_, bodyStart_, totalSize_ - bodyStart_);

	return std::move(*request_);
}

} // namespace ou::http
