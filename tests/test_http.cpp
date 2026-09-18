#define BOOST_TEST_MODULE ToyHttpServerTest
#include <boost/test/included/unit_test.hpp>

#include "AccessLog.h"
#include "HttpTypes.h"
#include "KVStore.h"
#include "Logging.h"
#include "Poller.h"
#include "RequestReader.h"
#include "Server.h"
#include "UniqueFd.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <sys/socket.h>

#ifndef DISABLE_HTTPS
#include <openssl/ssl.h>
#endif
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ou::http;

// --- Compile-time checks on the method and status tables ---

static_assert(stringToMethod("GET") == Method::GET);
static_assert(stringToMethod("DELETE") == Method::DELETE);
static_assert(!stringToMethod("BOGUS").has_value());
static_assert(!stringToMethod("get").has_value());
static_assert(methodToString(Method::PATCH) == "PATCH");
static_assert(defaultReasonPhrase(404) == "Not Found");
static_assert(defaultReasonPhrase(999) == "Unknown");

// --- Basic request and response tests ---

BOOST_AUTO_TEST_CASE(test_request_parsing) {
	const std::string rawRequest = "GET /index.html HTTP/1.1\r\n"
																 "Host: localhost\r\n"
																 "User-Agent: BoostTest\r\n"
																 "\r\n"
																 "Body content";

	const std::optional<Request> parsed = Request::parse(rawRequest);
	BOOST_REQUIRE(parsed.has_value());

	const Request &req = parsed.value();
	BOOST_CHECK_EQUAL(req.method, Method::GET);
	BOOST_CHECK_EQUAL(req.path, "/index.html");
	BOOST_CHECK_EQUAL(req.headers.at("Host"), "localhost");
	BOOST_CHECK_EQUAL(req.headers.at("User-Agent"), "BoostTest");
	BOOST_CHECK_EQUAL(req.body, "Body content");
}

BOOST_AUTO_TEST_CASE(test_request_parsing_rejects_malformed_input) {
	BOOST_CHECK(!Request::parse("BOGUS / HTTP/1.1\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET /index.html\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET index.html HTTP/1.1\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("").has_value());
	BOOST_CHECK(!Request::parse("\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET / HTTP/1.1\r\nNoColonHere\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET / HTTP/1.1\r\n: novalue\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET / HTTP/1.1\r\nContent-Length : 4\r\n\r\nabcd").has_value());
}

BOOST_AUTO_TEST_CASE(test_request_headers_are_case_insensitive) {
	const std::optional<Request> parsed = Request::parse("GET / HTTP/1.1\r\ncontent-length: 4\r\n\r\nabcd");
	BOOST_REQUIRE(parsed.has_value());

	const Request &req = parsed.value();
	BOOST_CHECK(req.header("Content-Length").has_value());
	BOOST_CHECK(req.header("CONTENT-LENGTH").has_value());
	BOOST_REQUIRE(req.contentLength().has_value());
	BOOST_CHECK_EQUAL(req.contentLength().value(), 4U);
}

BOOST_AUTO_TEST_CASE(test_request_content_length_rejects_junk) {
	const std::optional<Request> negative = Request::parse("GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n");
	BOOST_REQUIRE(negative.has_value());
	BOOST_CHECK(!negative.value().contentLength().has_value());

	const std::optional<Request> overflow = Request::parse("GET / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n");
	BOOST_REQUIRE(overflow.has_value());
	BOOST_CHECK(!overflow.value().contentLength().has_value());
}

BOOST_AUTO_TEST_CASE(test_query_string_is_split_from_path) {
	const std::optional<Request> parsed = Request::parse("GET /kv?key=hello&flag HTTP/1.1\r\n\r\n");
	BOOST_REQUIRE(parsed.has_value());

	const Request &req = parsed.value();
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

	const std::optional<Request> parsed = Request::parse("GET /my%20file.txt HTTP/1.1\r\n\r\n");
	BOOST_REQUIRE(parsed.has_value());
	BOOST_CHECK_EQUAL(parsed.value().path, "/my file.txt");

	const std::optional<Request> plus = Request::parse("GET /a+b.txt?q=a+b HTTP/1.1\r\n\r\n");
	BOOST_REQUIRE(plus.has_value());
	BOOST_CHECK_EQUAL(plus.value().path, "/a+b.txt");
	BOOST_CHECK_EQUAL(parseQuery(plus.value().query).at("q"), "a b");
}

BOOST_AUTO_TEST_CASE(test_percent_encoding) {
	BOOST_CHECK_EQUAL(percentEncode("a b"), "a%20b");
	BOOST_CHECK_EQUAL(percentEncode("/dir/a b.txt"), "/dir/a%20b.txt");
	// '+' must not survive: percentDecode reads it back as a space
	BOOST_CHECK_EQUAL(percentEncode("a+b"), "a%2Bb");
	BOOST_CHECK_EQUAL(percentEncode("a#b?c"), "a%23b%3Fc");
	BOOST_CHECK_EQUAL(percentEncode("aZ0-_.~/"), "aZ0-_.~/");

	BOOST_CHECK_EQUAL(percentDecode(percentEncode("/a b+c#d.txt")), "/a b+c#d.txt");
}

BOOST_AUTO_TEST_CASE(test_request_parsing_rejects_unsupported_versions) {
	BOOST_CHECK(Request::parse("GET / HTTP/1.0\r\n\r\n").has_value());
	BOOST_CHECK(Request::parse("GET / HTTP/1.1\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET / HTTP/2.0\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET / HTTP/9.9\r\n\r\n").has_value());
	BOOST_CHECK(!Request::parse("GET / HTTP/\r\n\r\n").has_value());
}

BOOST_AUTO_TEST_CASE(test_response_serialization) {
	Response res;
	res.statusCode = 200;
	res.headers["Content-Type"] = "text/plain";
	res.body = "Hello, world!";

	const std::string responseStr = res.serialize();
	// Reason phrase comes from the status code
	BOOST_CHECK(responseStr.find("HTTP/1.1 200 OK") != std::string::npos);
	BOOST_CHECK(responseStr.find("Content-Length: " + std::to_string(res.body.size())) != std::string::npos);
	BOOST_CHECK(responseStr.find("Content-Type: text/plain") != std::string::npos);
	BOOST_CHECK(responseStr.find("Hello, world!") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_response_emits_one_content_length) {
	Response res = Response::plainText(200, "abc");
	res.headers["Content-Length"] = "999";

	const std::string responseStr = res.serialize();
	BOOST_CHECK_EQUAL(responseStr.find("Content-Length: 3"), responseStr.rfind("Content-Length: 3"));
	BOOST_CHECK(responseStr.find("Content-Length: 999") == std::string::npos);
}

// --- Custom handler and middleware tests ---

class TestServer : public Server {
public:
	using Server::handleRequest;
	using Server::handleStaticFileRequest;
	using Server::Server;
};

class TestMiddleware : public Middleware {
public:
	bool process(Request &request, Response &response) override {
		if (request.path == "/middleware") {
			response = Response::plainText(200, "Intercepted by Middleware");
			return true;
		}
		return false;
	}
};

BOOST_AUTO_TEST_CASE(test_custom_handler_and_middleware) {
	TestServer::Config config;
	config.servingDirectory = ".";
	config.threadCount = 1;
	TestServer server(config);

	server.addMiddleware(std::make_shared<TestMiddleware>());
	server.registerPathHandler(Method::GET, "/test", [](const Request &) { return Response::plainText(200, "Handled by TestHandler"); });
	server.registerPatternHandler(Method::GET, R"(/pattern/\d+)", [](const Request &req) {
		(void)req;
		return Response::plainText(200, "Pattern function handler");
	});

	Request req1;
	req1.method = Method::GET;
	req1.path = "/test";
	BOOST_CHECK_EQUAL(server.handleRequest(req1).body, "Handled by TestHandler");

	Request req2;
	req2.method = Method::GET;
	req2.path = "/middleware";
	BOOST_CHECK_EQUAL(server.handleRequest(req2).body, "Intercepted by Middleware");

	Request req3;
	req3.method = Method::GET;
	req3.path = "/pattern/123";
	BOOST_CHECK_EQUAL(server.handleRequest(req3).body, "Pattern function handler");

	// Falls through to the static file handler
	Request req4;
	req4.method = Method::GET;
	req4.path = "/unknown";
	BOOST_CHECK_EQUAL(server.handleRequest(req4).statusCode, 404);
}

BOOST_AUTO_TEST_CASE(test_multi_method_registration) {
	TestServer::Config config;
	config.servingDirectory = ".";
	TestServer server(config);

	server.registerPathHandler({ Method::GET, Method::PUT }, "/multi",
														 [](const Request &req) { return Response::plainText(200, std::string(methodToString(req.method))); });

	Request get;
	get.method = Method::GET;
	get.path = "/multi";
	BOOST_CHECK_EQUAL(server.handleRequest(get).body, "GET");

	Request put;
	put.method = Method::PUT;
	put.path = "/multi";
	BOOST_CHECK_EQUAL(server.handleRequest(put).body, "PUT");

	Request post;
	post.method = Method::POST;
	post.path = "/multi";
	BOOST_CHECK_EQUAL(server.handleRequest(post).statusCode, 404);
}

// --- Static file serving ---

namespace {

std::filesystem::path sMakeTempDir(const std::string &name) {
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("toy_http_server_test_" + name);
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	return dir;
}

void sWriteFile(const std::filesystem::path &path, std::string_view content) {
	std::ofstream file(path);
	file << content;
}

std::filesystem::path sMakeTempLogPath(const std::string &name) {
	return std::filesystem::temp_directory_path() / ("toy_http_server_test_" + name + ".log");
}

Request sMakeLoggedRequest(const std::string &path) {
	Request request;
	request.method = Method::GET;
	request.path = path;

	sockaddr_in clientAddr{};
	clientAddr.sin_family = AF_INET;
	clientAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	request.clientAddr = clientAddr;

	return request;
}

size_t sCountLines(const std::filesystem::path &path) {
	std::ifstream file(path);
	size_t count = 0;
	std::string line;
	while (std::getline(file, line)) {
		++count;
	}
	return count;
}

Request sMakeGet(const std::string &path) {
	Request request;
	request.method = Method::GET;
	request.path = path;
	return request;
}

} // namespace

BOOST_AUTO_TEST_CASE(test_static_file_serving_and_mime_types) {
	const std::filesystem::path root = sMakeTempDir("static");
	sWriteFile(root / "page.html", "<h1>hi</h1>");
	sWriteFile(root / "notes.txt", "plain");
	sWriteFile(root / "blob.bin", "\x01\x02");

	TestServer::Config config;
	config.servingDirectory = root;
	TestServer server(config);

	const Response html = server.handleStaticFileRequest(sMakeGet("/page.html"));
	BOOST_CHECK_EQUAL(html.statusCode, 200);
	BOOST_CHECK_EQUAL(html.body, "<h1>hi</h1>");
	BOOST_CHECK_EQUAL(html.headers.at("Content-Type"), "text/html");

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeGet("/notes.txt")).headers.at("Content-Type"), "text/plain");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeGet("/blob.bin")).headers.at("Content-Type"), "application/octet-stream");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeGet("/missing.txt")).statusCode, 404);

	// Appears once
	const std::string wire = html.serialize();
	BOOST_CHECK_EQUAL(wire.find("Content-Length:"), wire.rfind("Content-Length:"));

	std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(test_directory_traversal_is_rejected) {
	const std::filesystem::path root = sMakeTempDir("traversal");
	sWriteFile(root / "public.txt", "public");
	sWriteFile(root.parent_path() / "toy_http_server_test_secret.txt", "secret");

	TestServer::Config config;
	config.servingDirectory = root;
	TestServer server(config);

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeGet("/public.txt")).statusCode, 200);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeGet("/../toy_http_server_test_secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeGet("/../../etc/passwd")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeGet("/subdir/../../etc/passwd")).statusCode, 403);

	// Encoded form decodes to the same traversal
	const std::optional<Request> encoded = Request::parse("GET /..%2Ftoy_http_server_test_secret.txt HTTP/1.1\r\n\r\n");
	BOOST_REQUIRE(encoded.has_value());
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(encoded.value()).statusCode, 403);

	std::filesystem::remove_all(root);
	std::filesystem::remove(root.parent_path() / "toy_http_server_test_secret.txt");
}

BOOST_AUTO_TEST_CASE(test_directory_indexing_escapes_names) {
	const std::filesystem::path root = sMakeTempDir("indexing");
	sWriteFile(root / "a<script>.txt", "x");
	sWriteFile(root / "a b.txt", "x");

	TestServer::Config indexingOff;
	indexingOff.servingDirectory = root;
	TestServer serverOff(indexingOff);
	BOOST_CHECK_EQUAL(serverOff.handleStaticFileRequest(sMakeGet("/")).statusCode, 403);

	TestServer::Config indexingOn;
	indexingOn.servingDirectory = root;
	indexingOn.enableDirectoryIndexing = true;
	TestServer serverOn(indexingOn);

	const Response index = serverOn.handleStaticFileRequest(sMakeGet("/"));
	BOOST_CHECK_EQUAL(index.statusCode, 200);
	BOOST_CHECK(index.body.find("&lt;script&gt;") != std::string::npos);
	BOOST_CHECK(index.body.find("<script>") == std::string::npos);

	BOOST_CHECK(index.body.find("href=\"/a%20b.txt\"") != std::string::npos);
	BOOST_CHECK(index.body.find(">a b.txt<") != std::string::npos);

	std::filesystem::remove_all(root);
}

// --- Key-value store ---

BOOST_AUTO_TEST_CASE(test_kv_store_uses_decoded_query) {
	KVStore store;

	Request put;
	put.method = Method::PUT;
	put.path = "/kv";
	put.query = "key=my%20key";
	put.body = "value";
	BOOST_CHECK_EQUAL(store.handle(put).statusCode, 200);
	BOOST_CHECK_EQUAL(store.size(), 1U);

	const std::optional<std::string> stored = store.get("my key");
	BOOST_REQUIRE(stored.has_value());
	BOOST_CHECK_EQUAL(stored.value(), "value");

	Request get;
	get.method = Method::GET;
	get.path = "/kv";
	get.query = "key=my+key";
	BOOST_CHECK_EQUAL(store.handle(get).body, "value");

	Request missingKey;
	missingKey.method = Method::GET;
	missingKey.path = "/kv";
	BOOST_CHECK_EQUAL(store.handle(missingKey).statusCode, 400);

	Request remove;
	remove.method = Method::DELETE;
	remove.path = "/kv";
	remove.query = "key=my%20key";
	BOOST_CHECK_EQUAL(store.handle(remove).statusCode, 200);
	BOOST_CHECK_EQUAL(store.size(), 0U);
	BOOST_CHECK_EQUAL(store.handle(remove).statusCode, 404);
}

// --- File descriptor ownership ---

BOOST_AUTO_TEST_CASE(test_unique_fd_closes_once) {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) open() is variadic
	ou::UniqueFd fd(::open("/dev/null", O_RDONLY));
	BOOST_REQUIRE(fd.valid());
	const int raw = fd.get();

	ou::UniqueFd moved(std::move(fd));
	BOOST_CHECK(moved.valid());
	// NOLINTNEXTLINE(bugprone-use-after-move) checking the moved-from state is the point
	BOOST_CHECK(!fd.valid());
	BOOST_CHECK_EQUAL(moved.get(), raw);

	moved.reset();
	BOOST_CHECK(!moved.valid());
	moved.reset(); // Idempotent

	errno = 0;
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) fcntl() is variadic
	BOOST_CHECK_EQUAL(::fcntl(raw, F_GETFD), -1);
	BOOST_CHECK_EQUAL(errno, EBADF);
}

// --- Poller ---

BOOST_AUTO_TEST_CASE(test_poller_readiness) {
	std::array<int, 2> pair{};
	BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()), 0);
	const ou::UniqueFd local(pair[0]);
	const ou::UniqueFd peer(pair[1]);

	ou::Poller poller;
	BOOST_REQUIRE(poller.valid());
	BOOST_REQUIRE(poller.add(local.get(), ou::Interest::Read));

	using namespace std::chrono_literals;
	BOOST_CHECK(poller.wait(10ms).empty());

	BOOST_REQUIRE_EQUAL(::write(peer.get(), "x", 1), 1);
	std::span<const ou::ReadyEvent> ready = poller.wait(500ms);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK_EQUAL(ready[0].fd, local.get());
	BOOST_CHECK(ready[0].readable);

	// Switching interest hides the still-unread byte and reports the empty send buffer instead
	BOOST_REQUIRE(poller.modify(local.get(), ou::Interest::Write));
	ready = poller.wait(500ms);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK(ready[0].writable);
	BOOST_CHECK(!ready[0].readable);

	BOOST_REQUIRE(poller.remove(local.get()));
	BOOST_CHECK(poller.wait(10ms).empty());
}

// --- Live socket tests ---

namespace {

ou::UniqueFd sConnect(uint16_t port) {
	ou::UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
	if (!fd)
		return {};

	// So a regression hangs this case rather than the whole ctest timeout
	const timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
	(void)::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
		return {};

	return fd;
}

// Length of one complete response, or nullopt while the headers or body are still incomplete
std::optional<size_t> sResponseLength(std::string_view response) {
	const size_t headerEnd = response.find("\r\n\r\n");
	if (headerEnd == std::string_view::npos)
		return std::nullopt;

	size_t length = headerEnd + 4;

	const std::string_view headers = response.substr(0, headerEnd);
	for (const auto line : std::views::split(headers, std::string_view("\r\n"))) {
		const std::string_view field(line.begin(), line.end());
		const size_t colon = field.find(':');
		if (colon == std::string_view::npos || !equalsIgnoreCase(field.substr(0, colon), "Content-Length"))
			continue;

		size_t bodySize = 0;
		const std::string_view value = trim(field.substr(colon + 1));
		if (std::from_chars(value.data(), value.data() + value.size(), bodySize).ec == std::errc{})
			length += bodySize;
		break;
	}

	return response.size() >= length ? std::optional(length) : std::nullopt;
}

// Reads exactly one response. Reading to EOF would instead wait out the keep-alive timeout, and
// the caller may have pipelined a second request whose response must stay in the socket.
std::string sReadResponse(std::invocable<std::span<char>> auto readChunk) {
	std::string response;
	std::array<char, 4096> chunk{};

	while (!sResponseLength(response)) {
		const ssize_t bytesRead = readChunk(std::span(chunk));
		if (bytesRead <= 0)
			break;
		response.append(chunk.data(), static_cast<size_t>(bytesRead));
	}

	return response;
}

std::string sReadResponse(int fd) {
	return sReadResponse([fd](std::span<char> buffer) { return ::recv(fd, buffer.data(), buffer.size(), 0); });
}

std::string sRoundTrip(uint16_t port, std::initializer_list<std::string_view> pieces) {
	const ou::UniqueFd fd = sConnect(port);
	if (!fd)
		return {};

	for (const std::string_view piece : pieces) {
		if (::send(fd.get(), piece.data(), piece.size(), 0) < 0)
			return {};

		// Long enough that the server sees each piece as its own read
		if (pieces.size() > 1)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	return sReadResponse(fd.get());
}

std::string sRoundTrip(uint16_t port, std::string_view rawRequest) { return sRoundTrip(port, { rawRequest }); }

} // namespace

BOOST_AUTO_TEST_CASE(test_server_round_trip_over_a_real_socket) {
	const std::filesystem::path root = sMakeTempDir("socket");
	sWriteFile(root / "hello.txt", "hello socket");

	Server::Config config;
	config.servingDirectory = root;
	config.port = 18173;
	config.threadCount = 2;

	Server server(config);
	BOOST_REQUIRE(server.start());

	const std::string ok = sRoundTrip(config.port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
	BOOST_CHECK(ok.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(ok.ends_with("hello socket"));

	const std::string head = sRoundTrip(config.port, "HEAD /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
	BOOST_CHECK(head.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(head.ends_with("\r\n\r\n"));
	BOOST_CHECK(head.find("Content-Length: 0\r\n") != std::string::npos);

	const std::string chunked
			= sRoundTrip(config.port, "POST /hello.txt HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
	BOOST_CHECK(chunked.starts_with("HTTP/1.1 501"));

	const std::string badVersion = sRoundTrip(config.port, "GET /hello.txt HTTP/2.0\r\nHost: localhost\r\n\r\n");
	BOOST_CHECK(badVersion.starts_with("HTTP/1.1 400"));

	// Must return rather than deadlock, and must tolerate the second call ~Server makes
	server.stop();
	server.stop();

	std::filesystem::remove_all(root);
}

// --- Keep-alive ---

BOOST_AUTO_TEST_CASE(test_request_reader_carries_pipelined_bytes) {
	RequestReader reader(1024);

	// Two requests in one buffer; the first must not swallow the second
	BOOST_REQUIRE(reader.consume("GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n") == RequestReader::Status::Complete);
	BOOST_CHECK_EQUAL(reader.take().path, "/a");

	BOOST_REQUIRE(reader.reset() == RequestReader::Status::Complete);
	BOOST_CHECK_EQUAL(reader.take().path, "/b");

	// Nothing left, so the next round has to wait for the socket
	BOOST_CHECK(reader.reset() == RequestReader::Status::NeedMore);
	BOOST_CHECK(reader.empty());
}

BOOST_AUTO_TEST_CASE(test_request_reader_body_does_not_absorb_next_request) {
	RequestReader reader(1024);

	BOOST_REQUIRE(reader.consume("POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nHELLOGET /b HTTP/1.1\r\nHost: x\r\n\r\n")
								== RequestReader::Status::Complete);
	const Request first = reader.take();
	BOOST_CHECK_EQUAL(first.path, "/a");
	BOOST_CHECK_EQUAL(first.body, "HELLO");

	BOOST_REQUIRE(reader.reset() == RequestReader::Status::Complete);
	BOOST_CHECK_EQUAL(reader.take().path, "/b");
}

BOOST_AUTO_TEST_CASE(test_keep_alive_defaults_by_version) {
	const std::optional<Request> oneOne = Request::parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_REQUIRE(oneOne.has_value());
	BOOST_CHECK(oneOne.value().wantsKeepAlive());

	const std::optional<Request> oneZero = Request::parse("GET / HTTP/1.0\r\nHost: x\r\n\r\n");
	BOOST_REQUIRE(oneZero.has_value());
	BOOST_CHECK(!oneZero.value().wantsKeepAlive());

	const std::optional<Request> closed = Request::parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
	BOOST_REQUIRE(closed.has_value());
	BOOST_CHECK(!closed.value().wantsKeepAlive());

	const std::optional<Request> revived = Request::parse("GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
	BOOST_REQUIRE(revived.has_value());
	BOOST_CHECK(revived.value().wantsKeepAlive());

	// The field is a comma-separated list, not a single token
	const std::optional<Request> listed = Request::parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: TE, Close\r\n\r\n");
	BOOST_REQUIRE(listed.has_value());
	BOOST_CHECK(!listed.value().wantsKeepAlive());
}

namespace {

// Both concurrency models must behave identically here, so each case runs against both
void sCheckKeepAlive(Server::Model model, uint16_t port) {
	const std::filesystem::path root = sMakeTempDir("keepalive");
	sWriteFile(root / "a.txt", "aaa");
	sWriteFile(root / "b.txt", "bbbb");

	Server::Config config;
	config.servingDirectory = root;
	config.port = port;
	config.threadCount = 2;
	config.model = model;

	Server server(config);
	BOOST_REQUIRE(server.start());

	{ // Two requests down one connection
		const ou::UniqueFd fd = sConnect(port);
		BOOST_REQUIRE(fd.valid());

		const std::string first = "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n";
		BOOST_REQUIRE(::send(fd.get(), first.data(), first.size(), 0) > 0);
		const std::string firstResponse = sReadResponse(fd.get());
		BOOST_CHECK(firstResponse.starts_with("HTTP/1.1 200 OK"));
		BOOST_CHECK(firstResponse.ends_with("aaa"));

		const std::string second = "GET /b.txt HTTP/1.1\r\nHost: x\r\n\r\n";
		BOOST_REQUIRE(::send(fd.get(), second.data(), second.size(), 0) > 0);
		const std::string secondResponse = sReadResponse(fd.get());
		BOOST_CHECK(secondResponse.starts_with("HTTP/1.1 200 OK"));
		BOOST_CHECK(secondResponse.ends_with("bbbb"));
	}

	{ // Pipelined: both requests arrive in one write, both get answered
		const ou::UniqueFd fd = sConnect(port);
		BOOST_REQUIRE(fd.valid());

		const std::string both = "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"
														 "GET /b.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
		BOOST_REQUIRE(::send(fd.get(), both.data(), both.size(), 0) > 0);

		BOOST_CHECK(sReadResponse(fd.get()).ends_with("aaa"));
		BOOST_CHECK(sReadResponse(fd.get()).ends_with("bbbb"));
	}

	{ // Connection: close is honoured, and the server really closes
		const ou::UniqueFd fd = sConnect(port);
		BOOST_REQUIRE(fd.valid());

		const std::string request = "GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
		BOOST_REQUIRE(::send(fd.get(), request.data(), request.size(), 0) > 0);

		const std::string response = sReadResponse(fd.get());
		BOOST_CHECK(response.find("Connection: close") != std::string::npos);

		std::array<char, 64> chunk{};
		BOOST_CHECK_EQUAL(::recv(fd.get(), chunk.data(), chunk.size(), 0), 0);
	}

	{ // HTTP/1.0 closes unless it asks otherwise
		const ou::UniqueFd fd = sConnect(port);
		BOOST_REQUIRE(fd.valid());

		const std::string request = "GET /a.txt HTTP/1.0\r\nHost: x\r\n\r\n";
		BOOST_REQUIRE(::send(fd.get(), request.data(), request.size(), 0) > 0);
		BOOST_CHECK(sReadResponse(fd.get()).find("Connection: close") != std::string::npos);

		std::array<char, 64> chunk{};
		BOOST_CHECK_EQUAL(::recv(fd.get(), chunk.data(), chunk.size(), 0), 0);
	}

	server.stop();
	std::filesystem::remove_all(root);
}

} // namespace

#ifndef DISABLE_HTTPS

namespace {

// A TLS client that does not verify the peer, since the demo cert is self-signed
struct TlsClient {
	SSL_CTX *ctx = nullptr;
	SSL *ssl = nullptr;
	ou::UniqueFd fd;

	explicit TlsClient(uint16_t port) : ctx(SSL_CTX_new(TLS_client_method())), fd(sConnect(port)) {
		if ((ctx == nullptr) || !fd.valid())
			return;

		ssl = SSL_new(ctx);
		if (ssl == nullptr)
			return;

		SSL_set_fd(ssl, fd.get());
		if (SSL_connect(ssl) != 1) {
			SSL_free(ssl);
			ssl = nullptr;
		}
	}

	TlsClient(const TlsClient &) = delete;
	TlsClient &operator=(const TlsClient &) = delete;
	TlsClient(TlsClient &&) = delete;
	TlsClient &operator=(TlsClient &&) = delete;

	~TlsClient() {
		if (ssl != nullptr) {
			SSL_shutdown(ssl);
			SSL_free(ssl);
		}
		SSL_CTX_free(ctx);
	}

	[[nodiscard]] bool connected() const { return ssl != nullptr; }

	[[nodiscard]] bool send(std::string_view request) const { return SSL_write(ssl, request.data(), static_cast<int>(request.size())) > 0; }

	[[nodiscard]] std::string receive() const {
		return sReadResponse([this](std::span<char> buffer) { return SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size())); });
	}
};

void sCheckConcurrentTlsSessions(uint16_t port) {
	std::vector<std::unique_ptr<TlsClient>> clients;
	clients.reserve(8);
	for (int i = 0; i < 8; ++i) {
		clients.push_back(std::make_unique<TlsClient>(port));
	}

	for (const auto &client : clients) {
		BOOST_REQUIRE(client->connected());
		BOOST_REQUIRE(client->send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
	}
	for (const auto &client : clients) {
		BOOST_CHECK(client->receive().ends_with("over tls"));
	}
}

// The event-loop handshake is resumable across wakeups, which nothing else exercises
void sCheckTls(Server::Model model, uint16_t port) {
	const std::filesystem::path root = sMakeTempDir("tls");
	sWriteFile(root / "a.txt", "over tls");

	Server::Config config;
	config.servingDirectory = root;
	config.port = port;
	config.threadCount = 2;
	config.model = model;
	config.https = { .enabled = true,
									 .certPath = std::filesystem::path(TEST_CERT_DIR) / "cert.pem",
									 .keyPath = std::filesystem::path(TEST_CERT_DIR) / "key.pem" };

	Server server(config);
	BOOST_REQUIRE(server.start());

	{ // Handshake, then a request
		TlsClient client(port);
		BOOST_REQUIRE(client.connected());
		BOOST_REQUIRE(client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"));

		const std::string response = client.receive();
		BOOST_CHECK(response.starts_with("HTTP/1.1 200 OK"));
		BOOST_CHECK(response.ends_with("over tls"));
	}

	{ // Keep-alive over TLS: two requests on one session
		TlsClient client(port);
		BOOST_REQUIRE(client.connected());

		BOOST_REQUIRE(client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
		BOOST_CHECK(client.receive().ends_with("over tls"));

		BOOST_REQUIRE(client.send("GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
		BOOST_CHECK(client.receive().starts_with("HTTP/1.1 404"));
	}

	// Thread-per-core cannot do this by construction: its 2 workers hold 2 keep-alive connections
	// open, so a third client's handshake has nobody to accept it until one times out.
	if (model == Server::Model::EventLoop)
		sCheckConcurrentTlsSessions(port);

	server.stop();
	std::filesystem::remove_all(root);
}

} // namespace

BOOST_AUTO_TEST_CASE(test_tls_thread_per_core) { sCheckTls(Server::Model::ThreadPerCore, 18180); }

BOOST_AUTO_TEST_CASE(test_tls_event_loop) { sCheckTls(Server::Model::EventLoop, 18181); }

#endif // DISABLE_HTTPS

namespace {

// A request that never parses returns before any Request exists, so it used to leave no trace
void sCheckRejectionIsLogged(Server::Model model, uint16_t port) {
	const std::filesystem::path root = sMakeTempDir("rejectlog");
	const std::filesystem::path logPath = sMakeTempLogPath("rejected");
	std::filesystem::remove(logPath);

	Server::Config config;
	config.servingDirectory = root;
	config.port = port;
	config.threadCount = 1;
	config.model = model;

	Server server(config);
	server.addMiddleware(std::make_shared<AccessLog>(AccessLog::Config{ .path = logPath, .flushBytes = 0 }));
	BOOST_REQUIRE(server.start());

	const std::string response = sRoundTrip(port, "BOGUS / HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(response.starts_with("HTTP/1.1 400"));

	server.stop();

	BOOST_REQUIRE(std::filesystem::exists(logPath));
	std::ifstream logFile(logPath);
	std::string entry;
	std::getline(logFile, entry);

	BOOST_CHECK(entry.find(" 400 ") != std::string::npos);
	BOOST_CHECK(entry.find("\"-\"") != std::string::npos);

	std::filesystem::remove(logPath);
	std::filesystem::remove_all(root);
}

} // namespace

BOOST_AUTO_TEST_CASE(test_rejection_is_logged_thread_per_core) { sCheckRejectionIsLogged(Server::Model::ThreadPerCore, 18182); }

BOOST_AUTO_TEST_CASE(test_rejection_is_logged_event_loop) { sCheckRejectionIsLogged(Server::Model::EventLoop, 18183); }

BOOST_AUTO_TEST_CASE(test_keep_alive_thread_per_core) { sCheckKeepAlive(Server::Model::ThreadPerCore, 18176); }

BOOST_AUTO_TEST_CASE(test_keep_alive_event_loop) { sCheckKeepAlive(Server::Model::EventLoop, 18177); }

BOOST_AUTO_TEST_CASE(test_keep_alive_can_be_disabled) {
	const std::filesystem::path root = sMakeTempDir("keepalive-off");
	sWriteFile(root / "a.txt", "aaa");

	Server::Config config;
	config.servingDirectory = root;
	config.port = 18178;
	config.threadCount = 1;
	config.keepAlive = false;

	Server server(config);
	BOOST_REQUIRE(server.start());

	const ou::UniqueFd fd = sConnect(config.port);
	BOOST_REQUIRE(fd.valid());

	const std::string request = "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n";
	BOOST_REQUIRE(::send(fd.get(), request.data(), request.size(), 0) > 0);
	BOOST_CHECK(sReadResponse(fd.get()).find("Connection: close") != std::string::npos);

	std::array<char, 64> chunk{};
	BOOST_CHECK_EQUAL(::recv(fd.get(), chunk.data(), chunk.size(), 0), 0);

	server.stop();
	std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(test_event_loop_serves_requests) {
	const std::filesystem::path root = sMakeTempDir("eventloop");
	sWriteFile(root / "hello.txt", "hello loop");
	// Larger than a socket buffer, so the response goes out over several writes
	sWriteFile(root / "large.txt", std::string(200000, 'x'));

	Server::Config config;
	config.servingDirectory = root;
	config.port = 18174;
	config.threadCount = 2;
	config.model = Server::Model::EventLoop;

	Server server(config);
	BOOST_REQUIRE(server.start());

	const std::string ok = sRoundTrip(config.port, "GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
	BOOST_CHECK(ok.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(ok.ends_with("hello loop"));

	const std::string missing = sRoundTrip(config.port, "GET /nope.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
	BOOST_CHECK(missing.starts_with("HTTP/1.1 404"));

	const std::string malformed = sRoundTrip(config.port, "BOGUS / HTTP/1.1\r\nHost: localhost\r\n\r\n");
	BOOST_CHECK(malformed.starts_with("HTTP/1.1 400"));

	const std::string large = sRoundTrip(config.port, "GET /large.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
	BOOST_CHECK(large.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK_EQUAL(large.size() - large.find("\r\n\r\n") - 4, 200000U);

	const std::string split = sRoundTrip(config.port, { "GET /hello.txt HTTP/1.1\r\nHost: ", "localhost\r\n", "\r\n" });
	BOOST_CHECK(split.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(split.ends_with("hello loop"));

	server.stop();
	server.stop();

	std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(test_event_loop_closes_idle_connection) {
	const std::filesystem::path root = sMakeTempDir("eventloop-idle");

	Server::Config config;
	config.servingDirectory = root;
	config.port = 18175;
	config.threadCount = 1;
	config.model = Server::Model::EventLoop;
	config.readTimeout = std::chrono::seconds(1);

	Server server(config);
	BOOST_REQUIRE(server.start());

	const ou::UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
	BOOST_REQUIRE(fd.valid());

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(config.port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	BOOST_REQUIRE_EQUAL(::connect(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);

	const timeval timeout{ .tv_sec = 10, .tv_usec = 0 };
	(void)::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	std::array<char, 64> chunk{};
	BOOST_CHECK_EQUAL(::recv(fd.get(), chunk.data(), chunk.size(), 0), 0);

	server.stop();
	std::filesystem::remove_all(root);
}

// --- Access log tests ---

BOOST_AUTO_TEST_CASE(test_access_log_middleware) {
	const std::filesystem::path logPath = sMakeTempLogPath("access");
	std::filesystem::remove(logPath);

	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 0 });

	Request request = sMakeLoggedRequest("/logged");
	Response response = Response::plainText(200, "hello");

	// process() never handles the request
	BOOST_CHECK(!accessLog.process(request, response));
	accessLog.postProcess(request, response);

	std::ifstream logFile(logPath);
	std::string entry;
	std::getline(logFile, entry);
	logFile.close();

	BOOST_CHECK(entry.find("127.0.0.1") != std::string::npos);
	BOOST_CHECK(entry.find("GET /logged") != std::string::npos);
	BOOST_CHECK(entry.find(" 200 5") != std::string::npos);
	BOOST_CHECK_EQUAL(sCountLines(logPath), 1U);

	// A request with no client address is not logged
	Request anonymousRequest;
	anonymousRequest.method = Method::GET;
	anonymousRequest.path = "/anonymous";
	accessLog.postProcess(anonymousRequest, response);
	BOOST_CHECK_EQUAL(sCountLines(logPath), 1U);

	std::filesystem::remove(logPath);
}

BOOST_AUTO_TEST_CASE(test_access_log_buffers_until_flushed) {
	const std::filesystem::path logPath = sMakeTempLogPath("buffered");
	std::filesystem::remove(logPath);

	const Response response = Response::plainText(200, "hello");

	{
		// Large enough that nothing reaches the file on its own, and an interval long enough that
		// the time-based flush cannot fire during the test
		AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = size_t{ 64 } * 1024, .flushInterval = std::chrono::hours(1) });

		for (int i = 0; i < 10; ++i) {
			Request request = sMakeLoggedRequest("/buffered");
			accessLog.postProcess(request, response);
		}

		BOOST_CHECK(!std::filesystem::exists(logPath));

		accessLog.flush();
		BOOST_CHECK_EQUAL(sCountLines(logPath), 10U);

		// Rotation still works while buffering, because each flush reopens by path
		std::filesystem::remove(logPath);
		Request request = sMakeLoggedRequest("/buffered");
		accessLog.postProcess(request, response);
		accessLog.flush();
		BOOST_REQUIRE(std::filesystem::exists(logPath));
		BOOST_CHECK_EQUAL(sCountLines(logPath), 1U);

		std::filesystem::remove(logPath);

		// Must survive destruction rather than being dropped
		Request last = sMakeLoggedRequest("/at-exit");
		accessLog.postProcess(last, response);
		BOOST_CHECK(!std::filesystem::exists(logPath));
	}

	BOOST_REQUIRE(std::filesystem::exists(logPath));
	BOOST_CHECK_EQUAL(sCountLines(logPath), 1U);

	std::filesystem::remove(logPath);
}

BOOST_AUTO_TEST_CASE(test_access_log_flushes_when_buffer_fills) {
	const std::filesystem::path logPath = sMakeTempLogPath("bufferfull");
	std::filesystem::remove(logPath);

	// One entry is well over 16 bytes, so every entry trips the size bound
	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 16, .flushInterval = std::chrono::hours(1) });
	const Response response = Response::plainText(200, "hello");

	for (int i = 0; i < 3; ++i) {
		Request request = sMakeLoggedRequest("/full");
		accessLog.postProcess(request, response);
	}

	BOOST_CHECK_EQUAL(sCountLines(logPath), 3U);
	std::filesystem::remove(logPath);
}

BOOST_AUTO_TEST_CASE(test_access_log_recreates_after_rotation) {
	const std::filesystem::path logPath = sMakeTempLogPath("rotation");
	std::filesystem::remove(logPath);

	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 0 });
	Request request = sMakeLoggedRequest("/rotated");
	Response response = Response::plainText(200, "hello");

	accessLog.postProcess(request, response);
	BOOST_CHECK_EQUAL(sCountLines(logPath), 1U);

	// Removed underneath us, as logrotate would
	std::filesystem::remove(logPath);
	accessLog.postProcess(request, response);

	BOOST_REQUIRE(std::filesystem::exists(logPath));
	BOOST_CHECK_EQUAL(sCountLines(logPath), 1U);

	std::filesystem::remove(logPath);
}

BOOST_AUTO_TEST_CASE(test_access_log_concurrent_writes) {
	const std::filesystem::path logPath = sMakeTempLogPath("concurrent");
	std::filesystem::remove(logPath);

	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 0 });
	Response response = Response::plainText(200, "hello");

	constexpr int threadCount = 4;
	constexpr int perThread = 25;

	std::vector<std::thread> threads;
	threads.reserve(threadCount);
	for (int i = 0; i < threadCount; ++i) {
		threads.emplace_back([&accessLog, &response]() {
			for (int j = 0; j < perThread; ++j) {
				Request request = sMakeLoggedRequest("/concurrent");
				accessLog.postProcess(request, response);
			}
		});
	}
	for (std::thread &thread : threads) {
		thread.join();
	}

	BOOST_CHECK_EQUAL(sCountLines(logPath), static_cast<size_t>(threadCount * perThread));

	std::filesystem::remove(logPath);
}

BOOST_AUTO_TEST_CASE(test_access_log_records_routed_status) {
	const std::filesystem::path logPath = sMakeTempLogPath("routed_status");
	std::filesystem::remove(logPath);

	TestServer::Config config;
	config.servingDirectory = ".";
	config.threadCount = 1;
	TestServer server(config);

	server.addMiddleware(std::make_shared<AccessLog>(AccessLog::Config{ .path = logPath, .flushBytes = 0 }));
	server.registerPathHandler({ Method::GET, Method::HEAD }, "/hello", [](const Request &) { return Response::plainText(200, "hi there"); });

	Request routed = sMakeLoggedRequest("/hello");
	const Response routedResponse = server.handleRequest(routed);
	BOOST_CHECK_EQUAL(routedResponse.statusCode, 200);
	BOOST_CHECK_EQUAL(routedResponse.body, "hi there");

	Request missing = sMakeLoggedRequest("/does-not-exist");
	const Response missingResponse = server.handleRequest(missing);
	BOOST_CHECK_EQUAL(missingResponse.statusCode, 404);

	std::ifstream logFile(logPath);
	std::string firstLine;
	std::string secondLine;
	std::getline(logFile, firstLine);
	std::getline(logFile, secondLine);
	logFile.close();

	BOOST_CHECK(firstLine.find("GET /hello") != std::string::npos);
	BOOST_CHECK(firstLine.find(" 200 8") != std::string::npos);

	BOOST_CHECK(secondLine.find("GET /does-not-exist") != std::string::npos);
	BOOST_CHECK(secondLine.find(" 404 ") != std::string::npos);
	BOOST_CHECK(secondLine.find(" 200 0") == std::string::npos);

	// HEAD drops the body, but the log records what the equivalent GET would have sent
	Request head = sMakeLoggedRequest("/hello");
	head.method = Method::HEAD;
	const Response headResponse = server.handleRequest(head);
	BOOST_CHECK(headResponse.body.empty());

	std::string thirdLine;
	std::ifstream reread(logPath);
	std::getline(reread, thirdLine);
	std::getline(reread, thirdLine);
	std::getline(reread, thirdLine);
	reread.close();
	BOOST_CHECK(thirdLine.find("HEAD /hello") != std::string::npos);
	BOOST_CHECK(thirdLine.find(" 200 8") != std::string::npos);

	BOOST_CHECK_EQUAL(sCountLines(logPath), 3U);

	std::filesystem::remove(logPath);
}
