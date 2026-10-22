// Demo driver

#include "KVStore.h"
#include "ou/http/AccessLog.h"
#include "ou/http/Awaitables.h"
#include "ou/http/Logging.h"
#include "ou/http/Router.h"
#include "ou/http/Server.h"
#include "ou/http/StaticFiles.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include <pthread.h>

namespace {

// Short-circuits writes before routing, e.g., PUT /kv
class ReadOnly final : public ou::http::Middleware {
public:
	std::optional<ou::http::Response> process(ou::http::Request &request) final {
		if (request.method == ou::http::Method::GET || request.method == ou::http::Method::HEAD)
			return std::nullopt;
		return ou::http::Response::plainText(405, "Server is read-only");
	}
};

class ServerHeader final : public ou::http::Middleware {
public:
	void postProcess(const ou::http::Request & /*request*/, ou::http::Response &response) final {
		response.headers["Server"] = "ou-http-server";
	}
};

class Args {
public:
	Args(int argc, char **argv) : args_(std::span(argv, static_cast<size_t>(argc)).subspan(1)) {}

	[[nodiscard]] bool has(std::string_view flag) const { return std::ranges::contains(args_, flag); }

	[[nodiscard]] unsigned number(std::string_view prefix) const {
		unsigned value = 0;
		for (const std::string_view arg : args_) {
			if (arg.starts_with(prefix))
				(void)std::from_chars(std::to_address(arg.begin() + prefix.size()), std::to_address(arg.end()), value);
		}
		return value;
	}

private:
	std::span<char *> args_;
};

// Blocks SIGINT and SIGTERM in this thread and every thread started later, so only wait() receives them
class StopSignals {
public:
	StopSignals() {
		sigemptyset(&signals_);
		sigaddset(&signals_, SIGINT);
		sigaddset(&signals_, SIGTERM);
		pthread_sigmask(SIG_BLOCK, &signals_, nullptr);
	}

	void wait() const {
		int received = 0;
		sigwait(&signals_, &received);
	}

private:
	sigset_t signals_{};
};

std::chrono::milliseconds sDemoDuration(int ms) { return std::chrono::milliseconds(std::clamp(ms, 0, 10'000)); }

ou::http::Router sBuildRouter(const Args &args, ou::http::StaticFiles files) {
	using namespace ou::http;

	Router router;
	router.fallback(std::move(files));

	if (!args.has("--no-access-log")) {
		const AccessLog::Config accessLogConfig = { .path = "access.log" };
		router.layer(std::make_shared<AccessLog>(accessLogConfig));
	}

	router.layer(std::make_shared<ServerHeader>());

	if (args.has("--read-only"))
		router.layer(std::make_shared<ReadOnly>());

	const auto kvStore = std::make_shared<KVStore>();
	router.route({ Method::GET, Method::PUT, Method::DELETE }, "/kv",
							 [kvStore](const Request &request, Query<"key"> key) { return kvStore->handle(request, key.value); });
	router.pattern({ Method::GET, Method::PUT, Method::DELETE }, "/kv/{key}",
								 [kvStore](const Request &request, Path<"key"> key) { return kvStore->handle(request, key.value); });

	router.route(Method::GET, "/whoami", [](Header<"User-Agent"> agent) { return std::move(agent.value); });

	router.route(Method::GET, "/sleep", [](Query<"ms", int> ms) -> Task<std::string> {
		co_await ou::http::sleepFor(sDemoDuration(ms.value));
		co_return "OK";
	});

	router.route(Method::GET, "/work", [](Query<"ms", int> ms) -> Task<std::string> {
		co_return co_await ou::http::offload([deadline = std::chrono::steady_clock::now() + sDemoDuration(ms.value)] {
			while (std::chrono::steady_clock::now() < deadline) {
			}
			return std::string("OK");
		});
	});

	return router;
}

} // namespace

int main(int argc, char **argv) {
	try {
		using namespace ou::http;

		const StopSignals stopSignals;
		(void)std::signal(SIGPIPE, SIG_IGN); // A peer that disconnects mid-write would otherwise kill the process

		const Args args(argc, argv);

		Server::Config config;
		config.model = args.has("--thread-per-core") ? Server::Model::ThreadPerCore : Server::Model::EventLoop;
		config.logRequests = !args.has("--no-request-log");
		config.handlerThreads = args.number("--handler-threads=");
		config.https = { .enabled = !args.has("--no-tls"), .certPath = "./example/certs/cert.pem", .keyPath = "./example/certs/key.pem" };

		const StaticFiles::Config filesConfig = { .directory = "./example/www",
																							.enableDirectoryIndexing = true,
																							.preloadBytes = args.has("--no-preload") ? 0 : size_t{ 64 } * 1024 * 1024,
																							.skipSymlinkCheck = args.has("--skip-symlink-check") };

		const std::expected<Server, Error> server = StaticFiles::create(filesConfig)
																										.transform(std::bind_front(sBuildRouter, std::cref(args)))
																										.and_then(std::bind_front(&Server::start, config));
		if (!server) {
			ou::logError("{}", server.error().message());
			return EXIT_FAILURE;
		}

		stopSignals.wait();

		ou::logInfo("Exiting");
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		ou::logError("Fatal error: {}", e.what());
		return EXIT_FAILURE;
	}
}
