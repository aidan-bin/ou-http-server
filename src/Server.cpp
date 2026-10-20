#include "ou/http/Server.h"
#include "Transport.h"
#include "ou/http/Logging.h"

#include <expected>
#include <format>
#include <memory>
#include <utility>

#include <netinet/in.h>

namespace ou::http {

Server::Server(Config config, Router router) : config_(std::move(config)), router_(std::move(router)) {}

Server::~Server() { stop(); }

std::expected<void, Error> Server::start() {
	logInfo("Starting server with {} threads...", config_.threadCount);

	auto transport = Transport::start(
			config_, [this](Request &request, const sockaddr_in &clientAddr) { return serve(request, clientAddr); },
			[this](int status, const sockaddr_in &clientAddr) { return rejectRequest(status, clientAddr); });
	if (!transport)
		return std::unexpected(std::move(transport).error());

	transport_ = std::move(*transport);
	port_ = transport_->port();
	return {};
}

void Server::stop() {
	if (!transport_)
		return;

	logInfo("Stopping server...");
	transport_.reset();

	logInfo("Server stopped");
}

Task<Response> Server::serve(Request &request, const sockaddr_in &clientAddr) const {
	request.clientAddr = clientAddr;

	// Taken before handling; middlewares may rewrite the request
	const bool keepAlive = config_.keepAlive && request.wantsKeepAlive();
	std::string logLine
			= config_.logRequests ? std::format("{} {} from {}", request.method, request.path, addressToString(clientAddr)) : std::string();

	Task<Response> handled = router_.handle(request);
	if (handled.done())
		return finalize(handled.result(), keepAlive, logLine);
	return finalizeLater(std::move(handled), keepAlive, std::move(logLine));
}

Response Server::finalize(Response response, bool keepAlive, std::string_view logLine) const {
	response.keepAlive = keepAlive;

	if (config_.logRequests)
		logInfo("{} -> {}", logLine, response.statusCode);
	return response;
}

Task<Response> Server::finalizeLater(Task<Response> handled, bool keepAlive, std::string logLine) const {
	Response response = co_await handled;
	co_return finalize(std::move(response), keepAlive, logLine);
}

Response Server::rejectRequest(int status, const sockaddr_in &clientAddr) const {
	logWarn("Rejecting request from {} with status {}", addressToString(clientAddr), status);

	Request request;
	request.clientAddr = clientAddr;

	return router_.postProcess(request, Response::error(status));
}

} // namespace ou::http
