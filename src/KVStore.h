#pragma once

#include "Server.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace ou::http {

class KVStore : public RequestHandler {
public:
	void set(const std::string &key, const std::string &value);

	std::optional<std::string> get(const std::string &key) const;

	// Return true if removed
	bool remove(const std::string &key);

	Response handle(const Request &request) override;

private:
	mutable std::mutex mutex_;
	std::unordered_map<std::string, std::string> store_;
};

} // namespace ou::http
