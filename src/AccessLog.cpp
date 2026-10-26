#include "ou/http/AccessLog.h"

#include <array>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <utility>

namespace ou::http {

namespace {
	std::string sGetTimestamp() {
		const auto now = std::chrono::system_clock::now();
		const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
		std::tm tmStruct{};
		localtime_r(&nowTime, &tmStruct);

		std::array<char, sizeof("YYYY-MM-DD HH:MM:SS")> buffer{};
		(void)std::strftime(buffer.data(), buffer.size(), "%F %T", &tmStruct);
		return buffer.data();
	}

	std::string sFormatLogEntry(const Request &request, const Response &response, const sockaddr_in &clientAddr) {
		const std::string requestLine = request.path.empty() ? "-" : std::format("{} {}", request.method, request.path);

		return std::format("[{}] {} - \"{}\" {} {}\n", sGetTimestamp(), addressToString(clientAddr), requestLine, response.statusCode,
		                   response.body.size());
	}
} // namespace

AccessLog::AccessLog(Config config) : config_(std::move(config)) {}

AccessLog::~AccessLog() { flush(); }

void AccessLog::postProcess(const Request &request, Response &response) {
	if (!request.clientAddr)
		return;

	const std::string entry = sFormatLogEntry(request, response, *request.clientAddr);
	const std::lock_guard<std::mutex> lock(mutex_);

	buffer_ += entry;

	// Size bounds memory; time bounds how far the file lags behind
	if (buffer_.size() >= config_.flushBytes || std::chrono::steady_clock::now() - lastFlush_ >= config_.flushInterval)
		flushLocked();
}

void AccessLog::flush() {
	const std::lock_guard<std::mutex> lock(mutex_);
	flushLocked();
}

void AccessLog::flushLocked() {
	lastFlush_ = std::chrono::steady_clock::now();
	if (buffer_.empty())
		return;

	std::ofstream logFile(config_.path, std::ios::app);

	if (logFile)
		logFile << buffer_;

	buffer_.clear();
}

} // namespace ou::http
