#include "KVStore.h"

#include <utility>

namespace ou::http {

void KVStore::set(std::string key, std::string value) {
	const std::lock_guard<std::mutex> lock(mutex_);
	store_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<std::string> KVStore::get(std::string_view key) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto it = store_.find(key);
	if (it != store_.end()) {
		return it->second;
	}
	return std::nullopt;
}

bool KVStore::remove(std::string_view key) {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto it = store_.find(key);
	if (it == store_.end()) {
		return false;
	}
	store_.erase(it);
	return true;
}

size_t KVStore::size() const {
	const std::lock_guard<std::mutex> lock(mutex_);
	return store_.size();
}

Response KVStore::handle(const Request &request) {
	const std::map<std::string, std::string> params = parseQuery(request.query);

	const auto keyIt = params.find("key");
	if (keyIt == params.end() || keyIt->second.empty()) {
		return Response::plainText(400, "Missing key parameter");
	}
	const std::string &key = keyIt->second;

	switch (request.method) {
		using enum Method;
	case GET: {
		const std::optional<std::string> value = get(key);
		if (value) {
			return Response::plainText(200, *value);
		}
		return Response::plainText(404, "Key not found");
	}
	case PUT:
		set(key, request.body);
		return Response::plainText(200, "OK");
	case DELETE:
		if (remove(key)) {
			return Response::plainText(200, "Deleted");
		}
		return Response::plainText(404, "Key not found");
	default:
		return Response::plainText(405, "Method Not Allowed");
	}
}

} // namespace ou::http
