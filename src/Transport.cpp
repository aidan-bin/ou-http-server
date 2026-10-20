#include "Transport.h"
#include "BlockingLoop.h"
#include "EventLoop.h"
#include "Listener.h"
#include "SSLSocketHandler.h"
#include "ou/http/Logging.h"

#include <exception>
#include <format>
#include <utility>

namespace ou::http {

namespace {

	Server::StartError sStartError(const SocketError &error) { return { .step = std::string(error.step), .code = error.code }; }

} // namespace

std::expected<std::unique_ptr<Transport>, Server::StartError> Transport::start(const Server::Config &config, Dispatch dispatch,
																																							 Reject reject) {
	std::unique_ptr<SocketHandler> socketHandler;
	try {
		if (config.https.enabled)
			socketHandler = std::make_unique<SSLSocketHandler>(config.https);
		else
			socketHandler = std::make_unique<PlainSocketHandler>();
	} catch (const std::exception &e) {
		return std::unexpected(Server::StartError{ .step = std::format("initialize socket handler: {}", e.what()), .code = {} });
	}

	std::expected<Listener, Server::StartError> listener = openListener(config.port).transform_error(sStartError);
	if (!listener)
		return std::unexpected(std::move(listener).error());

	logInfo("Server socket {} bound and listening on port {}...", listener->socket.get(), listener->port);
	return std::make_unique<Transport>(config, std::move(socketHandler), std::move(*listener), std::move(dispatch), std::move(reject));
}

Transport::Transport(const Server::Config &config, std::unique_ptr<SocketHandler> socketHandler, Listener listener, Dispatch dispatch,
										 Reject reject)
		: listenSocket_(std::move(listener.socket)), port_(listener.port), socketHandler_(std::move(socketHandler)),
			pool_(config.model == Server::Model::EventLoop && config.handlerThreads > 0 ? std::make_unique<HandlerPool>(config.handlerThreads)
																																									: nullptr),
			dispatch_(std::move(dispatch)), reject_(std::move(reject)), model_(config.model),
			loopConfig_{ .listenSocket = listenSocket_.get(), .maxRequestBytes = config.maxRequestBytes, .idleTimeout = config.readTimeout } {
	for (unsigned i = 0; i < config.threadCount; ++i)
		threads_.emplace_back([this](const std::stop_token &stop) { run(stop); });
}

Transport::~Transport() {
	if (pool_)
		pool_->stop();
	for (std::jthread &thread : threads_)
		thread.request_stop();
	threads_.clear();
}

void Transport::run(const std::stop_token &stop) {
	if (model_ == Server::Model::EventLoop) {
		EventLoop loop(loopConfig_, *socketHandler_, dispatch_, reject_, pool_.get());
		loop.run(stop);
		return;
	}

	BlockingLoop loop(loopConfig_, *socketHandler_, dispatch_, reject_);
	loop.run(stop);
}

} // namespace ou::http
