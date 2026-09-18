#include "RequestReader.h"

#include <algorithm>
#include <utility>

namespace ou::http {

RequestReader::Status RequestReader::reject(int status) noexcept {
	rejectStatus_ = status;
	return Status::Rejected;
}

RequestReader::Status RequestReader::consume(std::string_view bytes) {
	raw_.append(bytes);
	return parseBuffered();
}

RequestReader::Status RequestReader::reset() {
	// Anything past the finished request is the start of the next one
	raw_.erase(0, std::min(totalSize_, raw_.size()));
	headerEnd_ = std::string::npos;
	totalSize_ = 0;
	request_.reset();

	return parseBuffered();
}

RequestReader::Status RequestReader::parseBuffered() {
	if (raw_.empty())
		return Status::NeedMore;

	if (raw_.size() > maxRequestBytes_)
		return reject(413);

	if (headerEnd_ == std::string::npos) {
		headerEnd_ = raw_.find("\r\n\r\n");
		if (headerEnd_ == std::string::npos)
			return Status::NeedMore;
	}

	if (!request_) {
		request_ = Request::parse(raw_);
		if (!request_)
			return reject(400);

		if (request_->header("Transfer-Encoding"))
			return reject(501);

		totalSize_ = headerEnd_ + 4 + request_->contentLength().value_or(0);
		if (totalSize_ > maxRequestBytes_)
			return reject(413);
	}

	return raw_.size() < totalSize_ ? Status::NeedMore : Status::Complete;
}

Request RequestReader::take() {
	if (!request_)
		return {};

	// Truncate any bytes past Content-Length that arrived in the same read
	const size_t bodyStart = headerEnd_ + 4;
	request_->body.assign(raw_, bodyStart, totalSize_ - bodyStart);

	return std::move(*request_);
}

} // namespace ou::http
