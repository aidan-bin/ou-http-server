#include "Dispatcher.h"
#include "ou/http/Logging.h"

#include <format>
#include <string>
#include <utility>

namespace ou::http {

Dispatcher::Dispatcher(bool keepAlive, bool logRequests, Router router)
		: keepAlive_(keepAlive), logRequests_(logRequests), router_(std::move(router)) {}

Task<Response> Dispatcher::serve(Request &request, const sockaddr_in &clientAddr) const {
	request.clientAddr = clientAddr;

	// Taken before handling; middleware may rewrite the request
	const bool keepAlive = keepAlive_ && request.wantsKeepAlive();
	std::string logLine
			= logRequests_ ? std::format("{} {} from {}", request.method, request.path, addressToString(clientAddr)) : std::string();

	return then(router_.handle(request), [this, keepAlive, logLine = std::move(logLine)](Response response) {
		response.keepAlive = keepAlive;
		if (logRequests_)
			log::info("{} -> {}", logLine, response.statusCode);
		return response;
	});
}

Response Dispatcher::reject(int status, const sockaddr_in &clientAddr) const {
	log::warn("Rejecting request from {} with status {}", addressToString(clientAddr), status);

	Request request;
	request.clientAddr = clientAddr;

	return router_.finish(request, Response::error(status));
}

} // namespace ou::http
