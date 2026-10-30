#pragma once

#include "ou/http/Request.h"
#include "ou/http/Response.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace demo {

class KVStore {
public:
	void set(std::string key, std::string value);

	[[nodiscard]] std::optional<std::string> get(const std::string &key) const;

	// Returns true if removed
	bool remove(const std::string &key);

	[[nodiscard]] size_t size() const;

	// Serves GET, PUT and DELETE on key; the body is the value
	ou::http::Response handle(const ou::http::Request &request, const std::string &key);

private:
	mutable std::mutex mutex_;
	std::unordered_map<std::string, std::string> store_;
};

} // namespace demo
