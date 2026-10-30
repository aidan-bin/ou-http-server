#include "Transport.h"
#include "BlockingLoop.h"
#include "EventLoop.h"
#include "Listener.h"
#include "TlsIo.h"
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
		if (config.offloadThreads > 0 && config.model != Server::Model::EventLoop)
			return Error{ .step = "validate config: offloadThreads needs the event loop model", .code = {} };
		return std::nullopt;
	}

} // namespace

std::expected<std::unique_ptr<Transport>, Error> Transport::start(const Server::Config &config, Dispatcher dispatcher) {
	if (std::optional<Error> invalid = sConfigError(config))
		return std::unexpected(std::move(*invalid));

	std::unique_ptr<SocketIo> io;
	try {
		if (config.tls.enabled)
			io = std::make_unique<TlsIo>(config.tls.certPath, config.tls.keyPath);
		else
			io = std::make_unique<PlainIo>();
	} catch (const std::exception &e) {
		return std::unexpected(Error{ .step = std::format("initialize TLS: {}", e.what()), .code = {} });
	}

	std::expected<Listener, Error> listener = openListener(config.port);
	if (!listener)
		return std::unexpected(std::move(listener).error());

	log::info("Server socket {} bound and listening on port {} with {} threads...", listener->socket.get(), listener->port,
	          config.threadCount);
	return std::make_unique<Transport>(config, std::move(io), std::move(*listener), std::move(dispatcher));
}

Transport::Transport(const Server::Config &config, std::unique_ptr<SocketIo> io, Listener listener, Dispatcher dispatcher)
		: listenSocket_(std::move(listener.socket)), port_(listener.port), io_(std::move(io)),
			pool_(config.offloadThreads > 0 ? std::make_unique<ThreadPool>(config.offloadThreads) : nullptr), dispatcher_(std::move(dispatcher)),
			model_(config.model), context_{ .listenSocket = listenSocket_.get(),
	                                    .maxRequestBytes = config.maxRequestBytes,
	                                    .idleTimeout = config.idleTimeout,
	                                    .io = *io_,
	                                    .dispatcher = dispatcher_,
	                                    .pool = pool_.get() } {
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
		EventLoop loop(context_);
		loop.run(stop);
		return;
	}

	BlockingLoop loop(context_);
	loop.run(stop);
}

} // namespace ou::http
