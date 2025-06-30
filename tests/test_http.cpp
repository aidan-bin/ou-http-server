#define BOOST_TEST_MODULE ToyHttpServerTest
#include <boost/test/included/unit_test.hpp>

#include "HttpTypes.h"
#include "Server.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ou::http;

// --- Basic request and response tests ---

BOOST_AUTO_TEST_CASE(test_request_parsing) {
	std::string rawRequest = "GET /index.html HTTP/1.1\r\n"
													 "Host: localhost\r\n"
													 "User-Agent: BoostTest\r\n"
													 "\r\n"
													 "Body content";

	Request req = Request::parse(rawRequest);
	BOOST_CHECK_EQUAL(req.method, Method::GET);
	BOOST_CHECK_EQUAL(req.path, "/index.html");
	BOOST_CHECK_EQUAL(req.headers["Host"], "localhost");
	BOOST_CHECK_EQUAL(req.headers["User-Agent"], "BoostTest");
	BOOST_CHECK(req.body.find("Body content") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_response_serialization) {
	Response res;
	res.statusCode = 200;
	res.reasonPhrase = "OK";
	res.headers["Content-Type"] = "text/plain";
	res.body = "Hello, world!";

	std::string responseStr = res.serialize();
	BOOST_CHECK(responseStr.find("HTTP/1.1 200 OK") != std::string::npos);
	BOOST_CHECK(responseStr.find("Content-Length: " + std::to_string(res.body.size())) != std::string::npos);
	BOOST_CHECK(responseStr.find("Content-Type: text/plain") != std::string::npos);
	BOOST_CHECK(responseStr.find("Hello, world!") != std::string::npos);
}

// --- Custom handler and middleware tests ---

class TestServer : public Server {
public:
	using Server::handleRequest;
	using Server::Server;
};

class TestMiddleware : public Middleware {
public:
	bool process(Request &request, Response &response) override {
		if (request.path == "/middleware") {
			response = Response{ 200, "OK", { { "Content-Type", "text/plain" } }, "Intercepted by Middleware" };
			return true;
		}
		return false;
	}
};

class TestHandler : public RequestHandler {
public:
	Response handle(const Request &request) override {
		(void)request;
		return Response{ 200, "OK", { { "Content-Type", "text/plain" } }, "Handled by TestHandler" };
	}
};

BOOST_AUTO_TEST_CASE(test_custom_handler_and_middleware) {

	TestServer::Config config;
	config.servingDirectory = ".";
	config.threadCount = 1;
	TestServer server(config);

	server.addMiddleware(std::make_shared<TestMiddleware>());
	server.registerPathHandler(Method::GET, "/test", std::make_shared<TestHandler>());
	server.registerPatternHandler(Method::GET, R"(/pattern/\d+)", [](const Request &req) {
		(void)req;
		return Response{ 200, "Created", { { "Content-Type", "text/plain" } }, "Pattern function handler" };
	});

	// /test request is handled by TestHandler
	Request req1;
	req1.method = Method::GET;
	req1.path = "/test";
	auto resp1 = server.handleRequest(req1);
	BOOST_REQUIRE(resp1.has_value());
	BOOST_CHECK_EQUAL(resp1->statusCode, 200);
	BOOST_CHECK(resp1->body.find("Handled by TestHandler") != std::string::npos);

	// /middleware request is handled by TestMiddleware
	Request req2;
	req2.method = Method::GET;
	req2.path = "/middleware";
	auto resp2 = server.handleRequest(req2);
	BOOST_REQUIRE(resp2.has_value());
	BOOST_CHECK_EQUAL(resp2->statusCode, 200);
	BOOST_CHECK(resp2->body.find("Intercepted by Middleware") != std::string::npos);

	// /pattern/123 request is handled by pattern function handler
	Request req4;
	req4.method = Method::GET;
	req4.path = "/pattern/123";
	auto resp4 = server.handleRequest(req4);
	BOOST_REQUIRE(resp4.has_value());
	BOOST_CHECK_EQUAL(resp4->statusCode, 200);
	BOOST_CHECK(resp4->body.find("Pattern function handler") != std::string::npos);

	// Other requests fall through to static file handler
	Request req5;
	req5.method = Method::GET;
	req5.path = "/unknown";
	auto resp5 = server.handleRequest(req5);
	BOOST_REQUIRE(resp5.has_value());
	BOOST_CHECK_EQUAL(resp5->statusCode, 404);
}
