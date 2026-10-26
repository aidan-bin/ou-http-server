#include "Transport.h"
#include "BlockingLoop.h"
#include "EventLoop.h"
#include "Listener.h"
#include "SSLSocketHandler.h"
#include "ou/http/Logging.h"

#include <exception>
#include <format>
#include <optional>
#include <utility>

#include <csignal>
#include <pthread.h>

namespace ou::http {

namespace {

	std::optional<Error> sConfigError(const Server::Config &config) {
		if (config.threadCount == 0)
			return Error{ .step = "validate config: threadCount must be at least 1", .code = {} };
		if (config.handlerThreads > 0 && config.model != Server::Model::EventLoop)
			return Error{ .step = "validate config: handlerThreads needs the event loop model", .code = {} };
		return std::nullopt;
	}

} // namespace

std::expected<std::unique_ptr<Transport>, Error> Transport::start(const Server::Config &config, Dispatch dispatch, Reject reject) {
	if (std::optional<Error> invalid = sConfigError(config))
		return std::unexpected(std::move(*invalid));

	std::unique_ptr<SocketHandler> socketHandler;
	try {
		if (config.https.enabled)
			socketHandler = std::make_unique<SSLSocketHandler>(config.https.certPath, config.https.keyPath);
		else
			socketHandler = std::make_unique<PlainSocketHandler>();
	} catch (const std::exception &e) {
		return std::unexpected(Error{ .step = std::format("initialize socket handler: {}", e.what()), .code = {} });
	}

	std::expected<Listener, Error> listener = openListener(config.port);
	if (!listener)
		return std::unexpected(std::move(listener).error());

	log::info("Server socket {} bound and listening on port {} with {} threads...", listener->socket.get(), listener->port,
	          config.threadCount);
	return std::make_unique<Transport>(config, std::move(socketHandler), std::move(*listener), std::move(dispatch), std::move(reject));
}

Transport::Transport(const Server::Config &config, std::unique_ptr<SocketHandler> socketHandler, Listener listener, Dispatch dispatch,
                     Reject reject)
		: listenSocket_(std::move(listener.socket)), port_(listener.port), socketHandler_(std::move(socketHandler)),
			pool_(config.handlerThreads > 0 ? std::make_unique<HandlerPool>(config.handlerThreads) : nullptr), dispatch_(std::move(dispatch)),
			reject_(std::move(reject)), model_(config.model),
			loopConfig_{ .listenSocket = listenSocket_.get(), .maxRequestBytes = config.maxRequestBytes, .idleTimeout = config.idleTimeout } {
	for (unsigned i = 0; i < config.threadCount; ++i)
		threads_.emplace_back([this](const std::stop_token &stop) { run(stop); });
}

Transport::~Transport() {
	log::info("Stopping server...");
	if (pool_)
		pool_->stop();
	for (std::jthread &thread : threads_)
		thread.request_stop();
	threads_.clear();
	log::info("Server stopped");
}

void Transport::run(const std::stop_token &stop) {
	// A write to a closed peer returns EPIPE instead of killing the process
	sigset_t pipe{};
	sigemptyset(&pipe);
	sigaddset(&pipe, SIGPIPE);
	(void)pthread_sigmask(SIG_BLOCK, &pipe, nullptr);

	if (model_ == Server::Model::EventLoop) {
		EventLoop loop(loopConfig_, *socketHandler_, dispatch_, reject_, pool_.get());
		loop.run(stop);
		return;
	}

	BlockingLoop loop(loopConfig_, *socketHandler_, dispatch_, reject_);
	loop.run(stop);
}

} // namespace ou::http
