#include "ou/http/Server.h"
#include "Dispatcher.h"
#include "Transport.h"

#include <expected>
#include <memory>
#include <utility>

namespace ou::http {

Server::Server(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {}

Server::Server(Server &&) noexcept = default;

Server::~Server() = default;

std::expected<Server, Error> Server::start(const Config &config, Router router) {
	std::expected<std::unique_ptr<Transport>, Error> transport
			= Transport::start(config, Dispatcher(config.keepAlive, config.logRequests, std::move(router)));
	if (!transport)
		return std::unexpected(std::move(transport).error());

	return Server(*std::move(transport));
}

uint16_t Server::port() const noexcept { return transport_->port(); }

} // namespace ou::http
