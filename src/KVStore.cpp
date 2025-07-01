#include "KVStore.h"

#include <sstream>

namespace ou::http {

void KVStore::set(const std::string &key, const std::string &value) {
	std::lock_guard<std::mutex> lock(mutex_);
	store_[key] = value;
}

std::optional<std::string> KVStore::get(const std::string &key) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = store_.find(key);
	if (it != store_.end()) {
		return it->second;
	}
	return std::nullopt;
}

bool KVStore::remove(const std::string &key) {
	std::lock_guard<std::mutex> lock(mutex_);
	return store_.erase(key) > 0;
}

static std::optional<std::string> get_query_key(const std::string &path) {
	auto pos = path.find('?');
	if (pos == std::string::npos)
		return std::nullopt;
	std::string query = path.substr(pos + 1);
	std::istringstream iss(query);
	std::string pair;
	while (std::getline(iss, pair, '&')) {
		auto eq = pair.find('=');
		if (eq != std::string::npos && pair.substr(0, eq) == "key") {
			return pair.substr(eq + 1);
		}
	}
	return std::nullopt;
}

Response KVStore::handle(const Request &request) {
	auto keyOpt = get_query_key(request.path);
	if (!keyOpt) {
		return { 400, "Bad Request", { { "Content-Type", "text/plain" } }, "Missing key parameter" };
	}
	const std::string &key = *keyOpt;

	switch (request.method) {
	case Method::GET: {
		auto val = get(key);
		if (val) {
			return { 200, "OK", { { "Content-Type", "text/plain" } }, *val };
		}
		return { 404, "Not Found", { { "Content-Type", "text/plain" } }, "Key not found" };
	}
	case Method::PUT: {
		set(key, request.body);
		return { 200, "OK", { { "Content-Type", "text/plain" } }, "OK" };
	}
	case Method::DELETE: {
		if (remove(key)) {
			return { 200, "OK", { { "Content-Type", "text/plain" } }, "Deleted" };
		}
		return { 404, "Not Found", { { "Content-Type", "text/plain" } }, "Key not found" };
	}
	default:
		return { 405, "Method Not Allowed", { { "Content-Type", "text/plain" } }, "Method Not Allowed" };
	}
}

} // namespace ou::http