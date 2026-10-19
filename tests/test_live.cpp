#include "AccessLog.h"
#include "Client.h"
#include "Server.h"
#include "TestSupport.h"

#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace ou::http;
using namespace ou::test;
namespace bdata = boost::unit_test::data;

BOOST_FIXTURE_TEST_SUITE(live, ServingRoot)

BOOST_DATA_TEST_CASE(test_server_round_trip, bdata::make(kModels), model) {
	root.write("hello.txt", "hello socket");
	root.write("large.txt", std::string(200000, 'x'));

	Server server(config(model));
	BOOST_REQUIRE(server.start());
	const uint16_t port = server.port();

	const std::string ok = roundTrip(port, "GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(ok.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(ok.ends_with("hello socket"));

	const std::string head = roundTrip(port, "HEAD /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(head.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(head.ends_with("\r\n\r\n"));

	const std::string split = roundTrip(port, { "GET /hello.txt HTTP/1.1\r\nHost: ", "x\r\n", "\r\n" });
	BOOST_CHECK(split.ends_with("hello socket"));

	const std::string large = roundTrip(port, "GET /large.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(large.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK_EQUAL(large.size() - large.find("\r\n\r\n") - 4, 200000U);

	BOOST_CHECK(roundTrip(port, "GET /nope.txt HTTP/1.1\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 404"));
	BOOST_CHECK(roundTrip(port, "BOGUS / HTTP/1.1\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 400"));
	BOOST_CHECK(roundTrip(port, "GET /hello.txt HTTP/2.0\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 505"));
	BOOST_CHECK(
			roundTrip(port, "POST /hello.txt HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n").starts_with("HTTP/1.1 501"));

	server.stop();
	server.stop();
}

BOOST_DATA_TEST_CASE(test_slow_reader_gets_whole_response, bdata::make(kModels), model) {
	constexpr size_t kSize = size_t{ 4 } * 1024 * 1024; // Past the socket buffers, forcing partial writes
	root.write("huge.txt", std::string(kSize, 'x'));

	Server server(config(model));
	BOOST_REQUIRE(server.start());

	const Client client(server.port());
	client.send("GET /huge.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const std::string response = client.receive();
	BOOST_CHECK(response.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK_EQUAL(response.size() - response.find("\r\n\r\n") - 4, kSize);
}

BOOST_AUTO_TEST_CASE(test_start_reports_failed_step) {
	Server first(makeConfig("."));
	BOOST_REQUIRE(first.start());

	Server::Config taken = makeConfig(".");
	taken.port = first.port();
	Server second(taken);
	const auto started = second.start();
	BOOST_REQUIRE(!started);
	BOOST_CHECK_EQUAL(started.error().step, "bind socket");
	BOOST_CHECK(started.error().code == std::errc::address_in_use);

	root.write("file.txt", "not a directory");
	Server notDirectory(makeConfig(root.path() / "file.txt"));
	BOOST_CHECK(notDirectory.start().error().code == std::errc::not_a_directory);
	Server missing(makeConfig(root.path() / "missing"));
	BOOST_CHECK(missing.start().error().code == std::errc::no_such_file_or_directory);
}

BOOST_DATA_TEST_CASE(test_keep_alive, bdata::make(kModels), model) {
	root.write("a.txt", "aaa");
	root.write("b.txt", "bbbb");

	Server server(config(model));
	BOOST_REQUIRE(server.start());
	const uint16_t port = server.port();

	{ // Two requests down one connection
		const Client client(port);
		client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
		const std::string first = client.receive();
		BOOST_CHECK(first.starts_with("HTTP/1.1 200 OK"));
		BOOST_CHECK(first.ends_with("aaa"));

		client.send("GET /b.txt HTTP/1.1\r\nHost: x\r\n\r\n");
		BOOST_CHECK(client.receive().ends_with("bbbb"));
	}

	{ // Pipelined: both requests arrive in one write, both get answered
		const Client client(port);
		client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /b.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
		std::string both;
		for (std::string part = client.receive(); !part.empty(); part = client.receive())
			both += part;
		BOOST_CHECK(both.contains("aaaHTTP/1.1 200 OK"));
		BOOST_CHECK(both.ends_with("bbbb"));
	}

	{ // Connection: close is honoured, and the server really closes
		const Client client(port);
		client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
		BOOST_CHECK(client.receive().contains("Connection: close"));
		client.checkClosed();
	}

	{ // HTTP/1.0 closes unless it asks otherwise
		const Client client(port);
		client.send("GET /a.txt HTTP/1.0\r\nHost: x\r\n\r\n");
		BOOST_CHECK(client.receive().contains("Connection: close"));
		client.checkClosed();
	}
}

BOOST_DATA_TEST_CASE(test_keep_alive_can_be_disabled, bdata::make(kModels), model) {
	root.write("a.txt", "aaa");

	Server::Config closing = config(model);
	closing.keepAlive = false;
	Server server(closing);
	BOOST_REQUIRE(server.start());

	const Client client(server.port());
	client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(client.receive().contains("Connection: close"));
	client.checkClosed();
}

BOOST_DATA_TEST_CASE(test_rejection_is_logged, bdata::make(kModels), model) {
	const std::filesystem::path logPath = root.path() / "access.log";

	Server server(config(model));
	server.addMiddleware(std::make_shared<AccessLog>(AccessLog::Config{ .path = logPath, .flushBytes = 0 }));
	BOOST_REQUIRE(server.start());

	BOOST_CHECK(roundTrip(server.port(), "BOGUS / HTTP/1.1\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 400"));
	server.stop();

	const std::vector<std::string> logged = lines(logPath);
	BOOST_REQUIRE_EQUAL(logged.size(), 1U);
	BOOST_CHECK(logged[0].contains(" 400 "));
	BOOST_CHECK(logged[0].contains("\"-\""));
}

BOOST_AUTO_TEST_CASE(test_event_loop_closes_idle_connection) {
	Server::Config idle = config(Server::Model::EventLoop);
	idle.readTimeout = std::chrono::seconds(1);
	Server server(idle);
	BOOST_REQUIRE(server.start());

	Client(server.port()).checkClosed();
}

BOOST_AUTO_TEST_SUITE_END()
