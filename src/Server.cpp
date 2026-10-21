#include "ou/http/Server.h"
#include "Transport.h"
#include "ou/http/Logging.h"

#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <netinet/in.h>

namespace ou::http {

namespace {

	// Turns a parsed request into a finalized response, and answers rejections.
	// Immutable, so every loop thread can share it.
	class Dispatcher {
	public:
		Dispatcher(bool keepAlive, bool logRequests, Router router)
				: keepAlive_(keepAlive), logRequests_(logRequests), router_(std::move(router)) {}

		// Routes a request for a loop, then finalizes the response; request must outlive the task
		[[nodiscard]] Task<Response> serve(Request &request, const sockaddr_in &clientAddr) const {
			request.clientAddr = clientAddr;

			// Taken before handling; middlewares may rewrite the request
			const bool keepAlive = keepAlive_ && request.wantsKeepAlive();
			std::string logLine
					= logRequests_ ? std::format("{} {} from {}", request.method, request.path, addressToString(clientAddr)) : std::string();

			Task<Response> handled = router_.handle(request);
			if (handled.done())
				return finalize(handled.result(), keepAlive, logLine);
			return finalizeLater(std::move(handled), keepAlive, std::move(logLine));
		}

		// Logs and runs the layers' postProcess on a request that never parsed
		[[nodiscard]] Response reject(int status, const sockaddr_in &clientAddr) const {
			logWarn("Rejecting request from {} with status {}", addressToString(clientAddr), status);

			Request request;
			request.clientAddr = clientAddr;

			return router_.postProcess(request, Response::error(status));
		}

	private:
		// Adds keep-alive and the log line to a routed response
		[[nodiscard]] Response finalize(Response response, bool keepAlive, std::string_view logLine) const {
			response.keepAlive = keepAlive;

			if (logRequests_)
				logInfo("{} -> {}", logLine, response.statusCode);
			return response;
		}

		// Finalizes a handler that suspended, once it returns; capturing this is safe, since Transport joins the loops first
		[[nodiscard]] Task<Response> finalizeLater(Task<Response> handled, bool keepAlive, std::string logLine) const {
			Response response = co_await handled;
			co_return finalize(std::move(response), keepAlive, logLine);
		}

		bool keepAlive_;
		bool logRequests_;
		Router router_;
	};

} // namespace

Server::Server(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {}

Server::Server(Server &&) noexcept = default;

Server::~Server() = default;

std::expected<Server, Error> Server::start(const Config &config, Router router) {
	// Shared by both callbacks, since std::function needs them copyable; const, so every loop thread can use it
	const auto dispatcher = std::make_shared<const Dispatcher>(config.keepAlive, config.logRequests, std::move(router));
	std::expected<std::unique_ptr<Transport>, Error> transport
			= Transport::start(config, std::bind_front(&Dispatcher::serve, dispatcher), std::bind_front(&Dispatcher::reject, dispatcher));
	if (!transport)
		return std::unexpected(std::move(transport).error());

	return Server(*std::move(transport));
}

uint16_t Server::port() const noexcept { return transport_->port(); }

} // namespace ou::http
