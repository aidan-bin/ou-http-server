#pragma once

#include "ou/http/HttpTypes.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace demo {

// Heterogeneous lookup for allocation-free string_view keys
struct StringHash {
	using is_transparent = void;
	[[nodiscard]] size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};

class KVStore {
public:
	void set(std::string key, std::string value);

	[[nodiscard]] std::optional<std::string> get(std::string_view key) const;

	// Returns true if removed
	bool remove(std::string_view key);

	[[nodiscard]] size_t size() const;

	// Serves GET, PUT and DELETE on key; the body is the value
	ou::http::Response handle(const ou::http::Request &request, const std::string &key);

private:
	mutable std::mutex mutex_;
	std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> store_;
};

} // namespace demo
