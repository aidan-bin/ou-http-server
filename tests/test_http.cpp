#define BOOST_TEST_MODULE ToyHttpServerTest
#include <boost/test/included/unit_test.hpp>

#include "HttpTypes.h"
#include "Server.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ou::http;

BOOST_AUTO_TEST_CASE(test_request_parsing) {
	std::string rawRequest = "GET /index.html HTTP/1.1\r\n"
													 "Host: localhost\r\n"
													 "User-Agent: BoostTest\r\n"
													 "\r\n"
													 "Body content";

	Request req = Request::parse(rawRequest);
	BOOST_CHECK_EQUAL(req.method, "GET");
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
