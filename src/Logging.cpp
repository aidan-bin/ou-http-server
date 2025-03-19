#include "Logging.h"

#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

#include <arpa/inet.h>

namespace ou::http {

namespace {
	std::string sGetTimestamp() {
		auto now = std::chrono::system_clock::now();
		std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
		std::tm tmStruct;
		localtime_r(&nowTime, &tmStruct);

		std::ostringstream oss;
		oss << std::put_time(&tmStruct, "%Y-%m-%d %H:%M:%S");
		return oss.str();
	}

	std::string sFormatLogEntry(const Request& request, const Response& response, const sockaddr_in& clientAddr) {
		std::ostringstream oss;
		oss << "[" << sGetTimestamp() << "] "
			<< inet_ntoa(clientAddr.sin_addr) << " - \"" 
			<< request.method << " " << request.path << "\" "
			<< response.statusCode << " " << response.body.size() << "\n";
		return oss.str();
	}
}

AccessLog::AccessLog(const Config& config) : config_(config) {
	if (config_.enabled) {
		std::ofstream logFile(config_.path, std::ios::app);
	}
}

void AccessLog::log(const Request& request, const Response& response, const sockaddr_in& clientAddr) {
	if (!config_.enabled) return;

	std::ofstream logFile(config_.path, std::ios::app);
	if (!logFile) return;

	std::string entry = sFormatLogEntry(request, response, clientAddr);
	logFile << entry;
	logFile.close();

	enforceSizeLimit();
}

void AccessLog::enforceSizeLimit() {
    if (std::filesystem::file_size(config_.path) <= config_.maxSizeBytes) {
        return;
    }

    std::ifstream file(config_.path);
    std::deque<std::string> lines;
    std::string line;

    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    file.close();

    // Remove the oldest 20% of lines to limit truncation frequency
    size_t linesToRemove = lines.size() / 5;
    for (size_t i = 0; i < linesToRemove && !lines.empty(); ++i) {
        lines.pop_front();
    }

    // Write back to a temporary file and rename
    std::ofstream tempFile(config_.path.string() + ".tmp");
    for (const auto& l : lines) {
        tempFile << l << "\n";
    }
    tempFile.close();
    std::filesystem::rename(config_.path.string() + ".tmp", config_.path);
}

} // namespace ou::http