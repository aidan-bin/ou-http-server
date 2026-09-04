#include "Logging.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <arpa/inet.h>

namespace ou::http {

namespace {
	std::string sGetTimestamp() {
		auto now = std::chrono::system_clock::now();
		std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
		std::tm tmStruct{};
		localtime_r(&nowTime, &tmStruct);

		std::ostringstream oss;
		oss << std::put_time(&tmStruct, "%Y-%m-%d %H:%M:%S");
		return oss.str();
	}

	std::string sFormatLogEntry(const Request &request, const Response &response, const sockaddr_in &clientAddr) {
		std::ostringstream oss;
		oss << "[" << sGetTimestamp() << "] " << inet_ntoa(clientAddr.sin_addr) << " - \"" << request.method << " " << request.path << "\" "
				<< response.statusCode << " " << response.body.size() << "\n";
		return oss.str();
	}
} // namespace

AccessLog::AccessLog(Config config) : config_(std::move(config)) {}

void AccessLog::log(const Request &request, const Response &response, const sockaddr_in &clientAddr) {
	const std::lock_guard<std::mutex> lock(mutex_);

	// Opened per entry so an externally rotated log is recreated
	std::ofstream logFile(config_.path, std::ios::app);
	if (!logFile)
		return;

	logFile << sFormatLogEntry(request, response, clientAddr);
}

bool AccessLog::process(Request & /*request*/, Response & /*response*/) {
	// Logged from postProcess, once the status is known
	return false;
}

void AccessLog::postProcess(const Request &request, const Response &response) {
	if (request.clientAddr.has_value()) {
		log(request, response, request.clientAddr.value());
	}
}

} // namespace ou::http