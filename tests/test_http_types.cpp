#include "RequestReader.h"
#include "TestSupport.h"
#include "ou/http/HttpTypes.h"

#include <array>
#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>

using namespace ou::http;
using namespace ou::test;
namespace bdata = boost::unit_test::data;

static_assert(stringToMethod("GET") == Method::GET);
static_assert(stringToMethod("DELETE") == Method::DELETE);
static_assert(!stringToMethod("BOGUS").has_value());
static_assert(!stringToMethod("get").has_value());
static_assert(methodToString(Method::PATCH) == "PATCH");
static_assert(defaultReasonPhrase(404) == "Not Found");
static_assert(defaultReasonPhrase(999) == "Unknown");

BOOST_AUTO_TEST_SUITE(http_types)

BOOST_AUTO_TEST_CASE(test_request_parsing) {
	RequestReader reader(1024);
	BOOST_REQUIRE(reader.consume("GET /index.html HTTP/1.1\r\n"
	                             "Host: localhost\r\n"
	                             "User-Agent: BoostTest\r\n"
	                             "Content-Length: 12\r\n"
	                             "\r\n"
	                             "Body content")
	              == RequestReader::Status::Complete);

	const Request req = reader.take();
	BOOST_CHECK_EQUAL(req.method, Method::GET);
	BOOST_CHECK_EQUAL(req.path, "/index.html");
	BOOST_CHECK(req.header("Host") == "localhost");
	BOOST_CHECK(req.header("User-Agent") == "BoostTest");
	BOOST_CHECK_EQUAL(req.body, "Body content");

	BOOST_CHECK(!parseHead("").has_value());
}

constexpr auto kMalformedRequests = std::to_array<std::string_view>({
		"BOGUS / HTTP/1.1\r\n\r\n",
		"GET /index.html\r\n\r\n",
		"GET\r\n\r\n",
		"GET index.html HTTP/1.1\r\n\r\n",
		"\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: x\r\nNoColonHere\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: x\r\n: novalue\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: x\r\nContent-Length : 4\r\n\r\nabcd",
		"GET / HTTP/1.1\r\nHost: x\r\nContent-Length\t: 4\r\n\r\nabcd",
		"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\ncontent-length: 2\r\n\r\nab",
		"GET / HTTP/\r\n\r\n",
		"GET / HTTP/1\r\n\r\n",
		"GET / HTTP/1.10\r\n\r\n",
		"GET / http/1.1\r\n\r\n",
});

BOOST_DATA_TEST_CASE(test_malformed_requests_are_rejected, bdata::make(kMalformedRequests), raw) {
	BOOST_CHECK_EQUAL(parseHead(raw).error_or(0), 400);

	RequestReader reader(1024);
	BOOST_CHECK(reader.consume(raw) == RequestReader::Status::Rejected);
	BOOST_CHECK_EQUAL(reader.rejectStatus(), 400);
}

constexpr auto kUnsupportedVersions = std::to_array<std::string_view>({
		"GET / HTTP/2.0\r\n\r\n",
		"GET / HTTP/9.9\r\n\r\n",
		"GET / HTTP/0.9\r\n\r\n",
});

BOOST_DATA_TEST_CASE(test_unsupported_versions_are_rejected_with_505, bdata::make(kUnsupportedVersions), raw) {
	BOOST_CHECK_EQUAL(parseHead(raw).error_or(0), 505);

	RequestReader reader(1024);
	BOOST_CHECK(reader.consume(raw) == RequestReader::Status::Rejected);
	BOOST_CHECK_EQUAL(reader.rejectStatus(), 505);
}

BOOST_AUTO_TEST_CASE(test_request_headers_are_case_insensitive) {
	const Request req = parse("GET / HTTP/1.1\r\nHost: x\r\ncontent-length: 4\r\n\r\nabcd");
	BOOST_CHECK(req.header("Content-Length").has_value());
	BOOST_CHECK(req.header("CONTENT-LENGTH").has_value());
	BOOST_CHECK(req.contentLength() == std::optional<size_t>(4));
}

BOOST_AUTO_TEST_CASE(test_query_string_is_split_from_path) {
	const Request req = parse("GET /kv?key=hello&flag HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK_EQUAL(req.path, "/kv");
	BOOST_CHECK_EQUAL(req.query, "key=hello&flag");

	const std::map<std::string, std::string> params = parseQuery(req.query);
	BOOST_CHECK_EQUAL(params.at("key"), "hello");
	BOOST_CHECK_EQUAL(params.at("flag"), "");
}

BOOST_AUTO_TEST_CASE(test_percent_decoding) {
	BOOST_CHECK_EQUAL(percentDecode("a%20b"), "a b");
	BOOST_CHECK_EQUAL(percentDecode("a+b"), "a b");
	BOOST_CHECK_EQUAL(percentDecode("%2Fetc%2fpasswd"), "/etc/passwd");
	BOOST_CHECK_EQUAL(percentDecode("100%"), "100%");
	BOOST_CHECK_EQUAL(percentDecode("%zz"), "%zz");
	BOOST_CHECK_EQUAL(percentDecode("a+b", false), "a+b");

	BOOST_CHECK_EQUAL(parse("GET /my%20file.txt HTTP/1.1\r\nHost: x\r\n\r\n").path, "/my file.txt");

	const Request plus = parse("GET /a+b.txt?q=a+b HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK_EQUAL(plus.path, "/a+b.txt");
	BOOST_CHECK_EQUAL(parseQuery(plus.query).at("q"), "a b");
}

BOOST_AUTO_TEST_CASE(test_percent_encoding) {
	BOOST_CHECK_EQUAL(percentEncode("a b"), "a%20b");
	BOOST_CHECK_EQUAL(percentEncode("/dir/a b.txt"), "/dir/a%20b.txt");
	BOOST_CHECK_EQUAL(percentEncode("a+b"), "a%2Bb");
	BOOST_CHECK_EQUAL(percentEncode("a#b?c"), "a%23b%3Fc");
	BOOST_CHECK_EQUAL(percentEncode("aZ0-_.~/"), "aZ0-_.~/");

	BOOST_CHECK_EQUAL(percentDecode(percentEncode("/a b+c#d.txt")), "/a b+c#d.txt");

	std::string allBytes;
	for (int byte = 0; byte < 256; ++byte)
		allBytes += static_cast<char>(byte);
	BOOST_CHECK(percentDecode(percentEncode(allBytes)) == allBytes);
}

BOOST_AUTO_TEST_CASE(test_keep_alive_defaults_by_version) {
	BOOST_CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(!parse("GET / HTTP/1.0\r\nHost: x\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(!parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(parse("GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(!parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: TE, Close\r\n\r\n").wantsKeepAlive());
}

BOOST_AUTO_TEST_CASE(test_response_serialization) {
	Response res = Response::plainText(200, "Hello, world!");
	res.headers["Content-Length"] = "999";

	const std::string wire = res.serialize();
	BOOST_CHECK(wire.starts_with("HTTP/1.1 200 OK\r\n"));
	BOOST_CHECK(wire.contains("Content-Type: text/plain"));
	BOOST_CHECK(wire.ends_with("\r\n\r\nHello, world!"));

	BOOST_CHECK(wire.contains("Content-Length: 13\r\n"));
	BOOST_CHECK_EQUAL(wire.find("Content-Length:"), wire.rfind("Content-Length:"));
}

BOOST_AUTO_TEST_SUITE_END()
