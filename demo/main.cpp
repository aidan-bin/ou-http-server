// Demo driver

#include "Cli.h"
#include "KVStore.h"
#include "ou/http/http.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <expected>
#include <memory>
#include <optional>

namespace {

std::chrono::milliseconds sDemoDuration(int ms) { return std::chrono::milliseconds(std::clamp(ms, 0, 10'000)); }

ou::http::Router sBuildRouter(const demo::Args &args, ou::http::StaticFiles files) {
	using namespace ou::http;

	// Short-circuits writes before routing, e.g., PUT /kv
	const auto readOnly = [](Request &request) -> std::optional<Response> {
		if (request.method == Method::GET || request.method == Method::HEAD)
			return std::nullopt;
		Response response = Response::notAllowed("GET, HEAD");
		response.body = "Server is read-only";
		return response;
	};
	const auto sleep = [](Query<"ms", int> ms) -> Task<std::string> {
		co_await sleepFor(sDemoDuration(ms.value));
		co_return "OK";
	};
	const auto work = [](Query<"ms", int> ms) -> Task<std::string> {
		co_return co_await offload([deadline = std::chrono::steady_clock::now() + sDemoDuration(ms.value)] {
			while (std::chrono::steady_clock::now() < deadline) {
			}
			return std::string("OK");
		});
	};

	const auto store = std::make_shared<demo::KVStore>();
	const auto kv = [store](const Request &request, Query<"key"> key) {
		return store->handle(request, key.value);
	};
	const auto kvKey = [store](const Request &request, Path<"key"> key) {
		return store->handle(request, key.value);
	};

	Router router;
	if (!args.has("--no-access-log"))
		router.layer(std::make_shared<AccessLog>(AccessLog::Config{ .path = "access.log" }));
	router.layer(after([](const Request & /*request*/, Response &response) { response.headers["Server"] = "ou-http-server"; }));
	if (args.has("--read-only"))
		router.layer(before(readOnly));

	router.route("/kv", get(kv), put(kv), del(kv))
			.route("/kv/{key}", get(kvKey), put(kvKey), del(kvKey))
			.route("/whoami", get([](Header<"User-Agent"> agent) { return std::move(agent.value); }))
			.route("/sleep", get(sleep))
			.route("/work", get(work))
			.fallback(std::move(files));
	return router;
}

ou::http::Server::Config sServerConfig(const demo::Args &args) {
	using ou::http::Server;

	Server::Config config;
	config.model = args.has("--thread-per-core") ? Server::Model::ThreadPerCore : Server::Model::EventLoop;
	config.logRequests = !args.has("--no-request-log");
	config.handlerThreads = args.number("--handler-threads=");
	config.https = { .enabled = !args.has("--no-tls"), .certPath = "./example/certs/cert.pem", .keyPath = "./example/certs/key.pem" };
	return config;
}

ou::http::StaticFiles::Config sFilesConfig(const demo::Args &args) {
	return { .directory = "./example/www",
		       .enableDirectoryIndexing = true,
		       .preloadBytes = args.has("--no-preload") ? 0 : size_t{ 64 } * 1024 * 1024,
		       .skipSymlinkCheck = args.has("--skip-symlink-check") };
}

} // namespace

int main(int argc, char **argv) {
	using namespace ou::http;

	try {
		const demo::StopSignals stopSignals;

		const demo::Args args(argc, argv);

		const std::expected<StaticFiles, Error> files = StaticFiles::create(sFilesConfig(args));
		if (!files) {
			log::error("{}", files.error().message());
			return EXIT_FAILURE;
		}

		const std::expected<Server, Error> server = Server::start(sServerConfig(args), sBuildRouter(args, *files));
		if (!server) {
			log::error("{}", server.error().message());
			return EXIT_FAILURE;
		}

		stopSignals.wait();

		log::info("Exiting");
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		log::error("Fatal error: {}", e.what());
		return EXIT_FAILURE;
	}
}
