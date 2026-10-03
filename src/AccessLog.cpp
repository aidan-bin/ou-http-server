#include "AccessLog.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ou::http {

namespace {
	std::string sGetTimestamp() {
		const auto now = std::chrono::system_clock::now();
		const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
		std::tm tmStruct{};
		localtime_r(&nowTime, &tmStruct);

		std::ostringstream oss;
		oss << std::put_time(&tmStruct, "%Y-%m-%d %H:%M:%S");
		return oss.str();
	}

	std::string sFormatLogEntry(const Request &request, const Response &response, const sockaddr_in &clientAddr) {
		const std::string requestLine = request.path.empty() ? "-" : std::format("{} {}", request.method, request.path);

		return std::format("[{}] {} - \"{}\" {} {}\n", sGetTimestamp(), addressToString(clientAddr), requestLine, response.statusCode,
											 response.body.size());
	}
} // namespace

AccessLog::AccessLog(Config config) : config_(std::move(config)) {}

AccessLog::~AccessLog() { flush(); }

void AccessLog::log(const Request &request, const Response &response, const sockaddr_in &clientAddr) {
	// One instance is shared by every worker thread
	const std::lock_guard<std::mutex> lock(mutex_);

	buffer_ += sFormatLogEntry(request, response, clientAddr);

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

void AccessLog::postProcess(const Request &request, const Response &response) {
	// Middleware hook to log every request with its final status
	if (request.clientAddr.has_value())
		log(request, response, request.clientAddr.value());
}

} // namespace ou::http
