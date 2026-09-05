#define BOOST_TEST_MODULE ToyHttpServerTest
#include <boost/test/included/unit_test.hpp>

#include "AccessLog.h"
#include "HttpTypes.h"
#include "KVStore.h"
#include "Logging.h"
#include "Server.h"
#include "UniqueFd.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
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

// --- Live socket tests ---

namespace {

std::string sRoundTrip(uint16_t port, std::string_view rawRequest) {
	const ou::UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
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

	if (::send(fd.get(), rawRequest.data(), rawRequest.size(), 0) < 0)
		return {};

	std::string response;
	std::array<char, 4096> chunk{};
	while (true) {
		const ssize_t bytesRead = ::recv(fd.get(), chunk.data(), chunk.size(), 0);
		if (bytesRead <= 0)
			break;
		response.append(chunk.data(), static_cast<size_t>(bytesRead));
	}
	return response;
}

} // namespace

BOOST_AUTO_TEST_CASE(test_server_round_trip_over_a_real_socket) {
	const std::filesystem::path root = sMakeTempDir("socket");
	sWriteFile(root / "hello.txt", "hello socket");

	Server::Config config;
	config.servingDirectory = root;
	config.port = 18173;
	config.threadCount = 2;

	Server server(config);
	BOOST_REQUIRE(server.init());
	server.start();

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

// --- Access log tests ---

BOOST_AUTO_TEST_CASE(test_access_log_middleware) {
	const std::filesystem::path logPath = sMakeTempLogPath("access");
	std::filesystem::remove(logPath);

	AccessLog accessLog(AccessLog::Config{ .path = logPath });

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

BOOST_AUTO_TEST_CASE(test_access_log_recreates_after_rotation) {
	const std::filesystem::path logPath = sMakeTempLogPath("rotation");
	std::filesystem::remove(logPath);

	AccessLog accessLog(AccessLog::Config{ .path = logPath });
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

	AccessLog accessLog(AccessLog::Config{ .path = logPath });
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

	server.addMiddleware(std::make_shared<AccessLog>(AccessLog::Config{ .path = logPath }));
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
