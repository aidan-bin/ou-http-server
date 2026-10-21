#include "Client.h"
#include "TestSupport.h"
#include "ou/http/Awaitables.h"
#include "ou/http/HttpTypes.h"
#include "ou/http/Server.h"

#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace ou::http;
using namespace ou::test;
namespace bdata = boost::unit_test::data;

namespace {

constexpr auto kSlowHandler = std::chrono::milliseconds(400);

Task<Response> sNap(const Request & /*request*/) {
	co_await sleepFor(kSlowHandler);
	co_return Response::plainText(200, "rested");
}

Task<Response> sSlowWork(const Request & /*request*/) {
	co_return co_await offload([] {
		std::this_thread::sleep_for(kSlowHandler);
		return Response::plainText(200, "slow");
	});
}

Response sFast(const Request & /*request*/) { return Response::plainText(200, "fast"); }

void sCheckThrowingHandlerCloses(const Server::Config &config) {
	Router router;
	router.route(Method::GET, "/throw", [](const Request &) -> Response { throw std::runtime_error("handler failed"); });
	router.route(Method::GET, "/throw-after-sleep", [](const Request &) -> Task<Response> {
		co_await sleepFor(std::chrono::milliseconds(10));
		throw std::runtime_error("handler failed");
	});
	router.route(Method::GET, "/throw-offloaded", [](const Request &) -> Task<Response> {
		co_return co_await offload([]() -> Response { throw std::runtime_error("work failed"); });
	});
	const Server server = startServer(config, router);

	for (const std::string_view path : { "/throw", "/throw-after-sleep", "/throw-offloaded" }) {
		const Client client(server.port());
		client.send(std::format("GET {} HTTP/1.1\r\nHost: x\r\n\r\n", path));
		client.checkClosed();
	}
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(handlers_live, ServingRoot)

BOOST_DATA_TEST_CASE(test_handler_exception_closes_connection, bdata::make(kModels), model) { sCheckThrowingHandlerCloses(config(model)); }

BOOST_AUTO_TEST_CASE(test_handler_pool_exception_closes_connection) {
	Server::Config pooled = config(Server::Model::EventLoop);
	pooled.handlerThreads = 1;
	sCheckThrowingHandlerCloses(pooled);
}

BOOST_DATA_TEST_CASE(test_coroutine_handler_round_trip, bdata::make(kModels), model) {
	Router router;
	router.route(Method::GET, "/nap", sNap);
	const Server server = startServer(config(model), router);

	const std::string response = roundTrip(server.port(), "GET /nap HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(response.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(response.ends_with("rested"));
}

BOOST_AUTO_TEST_CASE(test_event_loop_sleeps_without_holding_its_thread) {
	Server::Config single = config(Server::Model::EventLoop);
	single.threadCount = 1;
	Router router;
	router.route(Method::GET, "/nap", sNap);
	const Server server = startServer(single, router);

	std::vector<Client> clients;
	clients.reserve(8);
	for (int i = 0; i < 8; ++i)
		clients.emplace_back(server.port());

	const auto start = std::chrono::steady_clock::now();
	for (const Client &client : clients)
		client.send("GET /nap HTTP/1.1\r\nHost: x\r\n\r\n");
	for (const Client &client : clients)
		BOOST_CHECK(client.receive().ends_with("rested"));
	BOOST_CHECK_LT(std::chrono::steady_clock::now() - start, kSlowHandler * 2);
}

BOOST_AUTO_TEST_CASE(test_event_loop_drops_a_sleep_whose_client_left) {
	Server::Config single = config(Server::Model::EventLoop);
	single.threadCount = 1;
	Router router;
	router.route(Method::GET, "/nap", sNap);
	router.route(Method::GET, "/fast", sFast);
	const Server server = startServer(single, router);

	Client(server.port()).send("GET /nap HTTP/1.1\r\nHost: x\r\n\r\n");

	const Client staying(server.port());
	staying.send("GET /nap HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(staying.receive().ends_with("rested"));
	BOOST_CHECK(roundTrip(server.port(), "GET /fast HTTP/1.1\r\nHost: x\r\n\r\n").ends_with("fast"));
}

BOOST_AUTO_TEST_CASE(test_handler_pool_keeps_slow_work_off_the_loop) {
	Server::Config pooled = config(Server::Model::EventLoop);
	pooled.handlerThreads = 2;
	pooled.threadCount = 1;
	Router router;
	router.route(Method::GET, "/slow", sSlowWork);
	router.route(Method::GET, "/fast", sFast);
	const Server server = startServer(pooled, router);

	const Client slow(server.port());
	slow.send("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	const auto start = std::chrono::steady_clock::now();
	BOOST_CHECK(roundTrip(server.port(), "GET /fast HTTP/1.1\r\nHost: x\r\n\r\n").ends_with("fast"));
	BOOST_CHECK_LT(std::chrono::steady_clock::now() - start, kSlowHandler / 2);

	BOOST_CHECK(slow.receive().ends_with("slow"));
}

BOOST_AUTO_TEST_CASE(test_handler_pool_stop_discards_queued_jobs) {
	Server::Config pooled = config(Server::Model::EventLoop);
	pooled.handlerThreads = 1;
	Router router;
	router.route(Method::GET, "/slow", sSlowWork);
	std::optional<Server> server = startServer(pooled, router);

	const Client running(server->port());
	const Client queued(server->port());
	running.send("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	queued.send("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	const auto start = std::chrono::steady_clock::now();
	server.reset(); // Discards the queued job
	BOOST_CHECK_LT(std::chrono::steady_clock::now() - start, kSlowHandler * 3 / 2);

	queued.checkClosed();
}

BOOST_AUTO_TEST_SUITE_END()
