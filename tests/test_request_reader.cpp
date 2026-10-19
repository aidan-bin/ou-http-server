#include "HttpTypes.h"
#include "ReadStream.h"
#include "RequestReader.h"

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ou::http;

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizer = true;
#elif defined(__has_feature)
constexpr bool kAddressSanitizer = __has_feature(address_sanitizer);
#else
constexpr bool kAddressSanitizer = false;
#endif

BOOST_AUTO_TEST_SUITE(request_reader)

BOOST_AUTO_TEST_CASE(test_request_reader_ignores_read_boundaries) {
	const std::vector<std::pair<std::string_view, std::vector<std::string>>> cases{
		{ "POST /a?q=1 HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nHELLOGET /b HTTP/1.1\r\n\r\n", { "POST /a q=1 h HELLO", "GET /b   " } },
		{ "GET /a HTTP/1.1\r\n\r\nGET /c HTTP/1.1\r\nHost: ", { "GET /a   ", "reject 408" } },
		{ "GET /a HTTP/1.1\r\n\r\nBOGUS / HTTP/1.1\r\n\r\n", { "GET /a   ", "reject 400" } },
		{ "GET / HTTP/1.1\r\nContent-Length: 18446744073709551560\r\n\r\n", { "reject 413" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: 5x\r\n\r\nGET /b HTTP/1.1\r\n\r\n", { "reject 400" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: -1\r\n\r\nGET /b HTTP/1.1\r\n\r\n", { "reject 400" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\nGET /b HTTP/1.1\r\n\r\n", { "reject 400" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", { "POST /a   x" } },
	};

	for (const auto &[stream, expected] : cases) {
		for (size_t chunkSize = 1; chunkSize <= stream.size(); ++chunkSize) {
			BOOST_TEST_CONTEXT("stream " << stream << ", chunk " << chunkSize) {
				BOOST_TEST(readStream(stream, chunkSize) == expected, boost::test_tools::per_element());
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(test_request_reader_enforces_size_limit) {
	const std::string head = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n";

	RequestReader fits(head.size() + 10);
	BOOST_CHECK(fits.consume(head + std::string(10, 'a')) == RequestReader::Status::Complete);

	RequestReader over(head.size() + 9);
	BOOST_CHECK(over.consume(head) == RequestReader::Status::Rejected);
	BOOST_CHECK_EQUAL(over.rejectStatus(), 413);

	RequestReader flood(1024);
	BOOST_CHECK(flood.consume("GET / HTTP/1.1\r\nX: " + std::string(1024, 'a')) == RequestReader::Status::Rejected);
	BOOST_CHECK_EQUAL(flood.rejectStatus(), 413);
}

BOOST_AUTO_TEST_CASE(test_request_views_survive_body_reads) {
	const std::string body(3000, 'b');
	RequestReader reader(4096);
	BOOST_REQUIRE(reader.consume("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 3000\r\n\r\n") == RequestReader::Status::NeedMore);
	for (size_t offset = 0; offset < body.size(); offset += 100)
		(void)reader.consume(std::string_view(body).substr(offset, 100));

	const Request request = reader.take();
	BOOST_CHECK(request.header("Host") == "x");
	BOOST_CHECK_EQUAL(request.body, body);
}

BOOST_AUTO_TEST_CASE(test_request_views_survive_body_with_pipelined_bytes) {
	RequestReader reader(8192);
	BOOST_REQUIRE(reader.consume("POST /kv?key=a HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n") == RequestReader::Status::NeedMore);
	const std::string next = "hello" + std::string("GET /next HTTP/1.1\r\nHost: ") + std::string(4000, 'y') + "\r\n\r\n";
	BOOST_REQUIRE(reader.consume(next) == RequestReader::Status::Complete);

	const Request request = reader.take();
	BOOST_CHECK_EQUAL(request.query, "key=a");
	BOOST_CHECK(request.header("Host") == "x");
	BOOST_CHECK_EQUAL(request.body, "hello");
}

BOOST_AUTO_TEST_CASE(test_request_reader_dribbled_head_is_linear) {
	std::string stream = "GET / HTTP/1.1\r\n";
	while (stream.size() < 64 * size_t{ 1024 })
		stream += "a:b\r\n";
	stream += "\r\n";

	RequestReader reader(stream.size());
	const auto start = std::chrono::steady_clock::now();
	RequestReader::Status status{};
	for (const char byte : stream)
		status = reader.consume({ &byte, 1 });
	BOOST_CHECK(status == RequestReader::Status::Complete);
	// Complexity check: linear takes ~10ms, the old quadratic rescan ~10s. ASan takes ~4s even when linear
	if constexpr (!kAddressSanitizer)
		BOOST_CHECK_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

BOOST_AUTO_TEST_SUITE_END()
