#define BOOST_TEST_MODULE OuHttpServerTest
#include <boost/test/data/test_case.hpp>
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
#include <format>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <ostream>
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
namespace bdata = boost::unit_test::data;

namespace ou::http {
std::ostream &operator<<(std::ostream &os, Server::Model model) {
	return os << (model == Server::Model::EventLoop ? "EventLoop" : "ThreadPerCore");
}
} // namespace ou::http

namespace {

constexpr std::array kModels{ Server::Model::ThreadPerCore, Server::Model::EventLoop };

class TempDir {
public:
	explicit TempDir(std::string_view name) : path_(std::filesystem::temp_directory_path() / std::format("ou_http_server_test_{}", name)) {
		std::filesystem::remove_all(path_);
		std::filesystem::create_directories(path_);
	}
	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;
	TempDir(TempDir &&) = delete;
	TempDir &operator=(TempDir &&) = delete;
	~TempDir() {
		std::error_code ignored;
		std::filesystem::remove_all(path_, ignored);
	}

	[[nodiscard]] const std::filesystem::path &path() const { return path_; }

	void write(const std::filesystem::path &name, std::string_view content) const { std::ofstream(path_ / name) << content; }

private:
	std::filesystem::path path_;
};

std::vector<std::string> sLines(const std::filesystem::path &path) {
	std::ifstream file(path);
	std::vector<std::string> lines;
	for (std::string line; std::getline(file, line);)
		lines.push_back(line);
	return lines;
}

bool sContains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

Request sParse(std::string_view raw) {
	std::optional<Request> parsed = Request::parse(raw);
	BOOST_REQUIRE(parsed.has_value());
	return std::move(*parsed);
}

Request sMakeRequest(const std::string &path, Method method = Method::GET) {
	Request request;
	request.method = method;
	request.path = path;

	sockaddr_in clientAddr{};
	clientAddr.sin_family = AF_INET;
	clientAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	request.clientAddr = clientAddr;

	return request;
}

Server::Config sConfig(const std::filesystem::path &root, Server::Model model = Server::Model::ThreadPerCore) {
	Server::Config config;
	config.servingDirectory = root;
	config.port = 0; // Kernel picks
	config.threadCount = 2;
	config.model = model;
	return config;
}

} // namespace

// --- Compile-time checks on the method and status tables ---

static_assert(stringToMethod("GET") == Method::GET);
static_assert(stringToMethod("DELETE") == Method::DELETE);
static_assert(!stringToMethod("BOGUS").has_value());
static_assert(!stringToMethod("get").has_value());
static_assert(methodToString(Method::PATCH) == "PATCH");
static_assert(defaultReasonPhrase(404) == "Not Found");
static_assert(defaultReasonPhrase(999) == "Unknown");

// --- Parsing ---

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
	BOOST_CHECK_EQUAL(req.headers.at("Host"), "localhost");
	BOOST_CHECK_EQUAL(req.headers.at("User-Agent"), "BoostTest");
	BOOST_CHECK_EQUAL(req.body, "Body content");

	BOOST_CHECK(!Request::parse("").has_value());
}

constexpr auto kMalformedRequests = std::to_array<std::string_view>({
		"BOGUS / HTTP/1.1\r\n\r\n",
		"GET /index.html\r\n\r\n",
		"GET\r\n\r\n",
		"GET index.html HTTP/1.1\r\n\r\n",
		"\r\n\r\n",
		"GET / HTTP/1.1\r\nNoColonHere\r\n\r\n",
		"GET / HTTP/1.1\r\n: novalue\r\n\r\n",
		"GET / HTTP/1.1\r\nContent-Length : 4\r\n\r\nabcd",
		"GET / HTTP/1.1\r\nContent-Length\t: 4\r\n\r\nabcd",
		"POST / HTTP/1.1\r\nContent-Length: 1\r\ncontent-length: 2\r\n\r\nab",
		"GET / HTTP/2.0\r\n\r\n",
		"GET / HTTP/9.9\r\n\r\n",
		"GET / HTTP/\r\n\r\n",
});

BOOST_DATA_TEST_CASE(test_malformed_requests_are_rejected, bdata::make(kMalformedRequests), raw) {
	BOOST_CHECK(!Request::parse(raw).has_value());

	RequestReader reader(1024);
	BOOST_CHECK(reader.consume(raw) == RequestReader::Status::Rejected);
	BOOST_CHECK_EQUAL(reader.rejectStatus(), 400);
}

BOOST_AUTO_TEST_CASE(test_request_headers_are_case_insensitive) {
	const Request req = sParse("GET / HTTP/1.1\r\ncontent-length: 4\r\n\r\nabcd");
	BOOST_CHECK(req.header("Content-Length").has_value());
	BOOST_CHECK(req.header("CONTENT-LENGTH").has_value());
	BOOST_CHECK(req.contentLength() == std::optional<size_t>(4));
}

BOOST_AUTO_TEST_CASE(test_query_string_is_split_from_path) {
	const Request req = sParse("GET /kv?key=hello&flag HTTP/1.1\r\n\r\n");
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

	BOOST_CHECK_EQUAL(sParse("GET /my%20file.txt HTTP/1.1\r\n\r\n").path, "/my file.txt");

	const Request plus = sParse("GET /a+b.txt?q=a+b HTTP/1.1\r\n\r\n");
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
}

BOOST_AUTO_TEST_CASE(test_keep_alive_defaults_by_version) {
	BOOST_CHECK(sParse("GET / HTTP/1.1\r\nHost: x\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(!sParse("GET / HTTP/1.0\r\nHost: x\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(!sParse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(sParse("GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n").wantsKeepAlive());
	BOOST_CHECK(!sParse("GET / HTTP/1.1\r\nHost: x\r\nConnection: TE, Close\r\n\r\n").wantsKeepAlive());
}

BOOST_AUTO_TEST_CASE(test_response_serialization) {
	Response res = Response::plainText(200, "Hello, world!");
	res.headers["Content-Length"] = "999";

	const std::string wire = res.serialize();
	BOOST_CHECK(wire.starts_with("HTTP/1.1 200 OK\r\n"));
	BOOST_CHECK(sContains(wire, "Content-Type: text/plain"));
	BOOST_CHECK(wire.ends_with("\r\n\r\nHello, world!"));

	BOOST_CHECK(sContains(wire, "Content-Length: 13\r\n"));
	BOOST_CHECK_EQUAL(wire.find("Content-Length:"), wire.rfind("Content-Length:"));
}

// --- RequestReader ---

namespace {

std::vector<std::string> sReadStream(std::string_view stream, size_t chunkSize) {
	RequestReader reader(1024);
	std::vector<std::string> events;

	auto drain = [&](RequestReader::Status status) {
		while (status == RequestReader::Status::Complete) {
			const Request request = reader.take();
			events.push_back(std::format("{} {} {}", request.method, request.path, request.body));
			status = reader.reset();
		}
		if (status == RequestReader::Status::Rejected)
			events.push_back(std::format("reject {}", reader.rejectStatus()));
		return status != RequestReader::Status::Rejected;
	};

	for (size_t offset = 0; offset < stream.size(); offset += chunkSize) {
		if (!drain(reader.consume(stream.substr(offset, chunkSize))))
			return events;
	}
	drain(reader.abandon());
	return events;
}

} // namespace

BOOST_AUTO_TEST_CASE(test_request_reader_ignores_read_boundaries) {
	const std::vector<std::pair<std::string_view, std::vector<std::string>>> cases{
		{ "POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nHELLOGET /b HTTP/1.1\r\n\r\n", { "POST /a HELLO", "GET /b " } },
		{ "GET /a HTTP/1.1\r\n\r\nGET /c HTTP/1.1\r\nHost: ", { "GET /a ", "reject 408" } },
		{ "GET /a HTTP/1.1\r\n\r\nBOGUS / HTTP/1.1\r\n\r\n", { "GET /a ", "reject 400" } },
		{ "GET / HTTP/1.1\r\nContent-Length: 18446744073709551560\r\n\r\n", { "reject 413" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: 5x\r\n\r\nGET /b HTTP/1.1\r\n\r\n", { "reject 400" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: -1\r\n\r\nGET /b HTTP/1.1\r\n\r\n", { "reject 400" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\nGET /b HTTP/1.1\r\n\r\n", { "reject 400" } },
		{ "POST /a HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", { "POST /a x" } },
	};

	for (const auto &[stream, expected] : cases) {
		for (size_t chunkSize = 1; chunkSize <= stream.size(); ++chunkSize) {
			BOOST_TEST_CONTEXT("stream " << stream << ", chunk " << chunkSize) {
				BOOST_TEST(sReadStream(stream, chunkSize) == expected, boost::test_tools::per_element());
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

// --- Routing ---

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
	TestServer server(sConfig("."));

	server.addMiddleware(std::make_shared<TestMiddleware>());
	server.registerPathHandler(Method::GET, "/test", [](const Request &) { return Response::plainText(200, "Handled by TestHandler"); });
	server.registerPatternHandler(Method::GET, R"(/pattern/\d+)",
																[](const Request &) { return Response::plainText(200, "Pattern handler"); });

	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/test")).body, "Handled by TestHandler");
	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/middleware")).body, "Intercepted by Middleware");
	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/pattern/123")).body, "Pattern handler");
	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/unknown")).statusCode, 404);
}

BOOST_AUTO_TEST_CASE(test_multi_method_registration) {
	TestServer server(sConfig("."));
	server.registerPathHandler({ Method::GET, Method::PUT }, "/multi",
														 [](const Request &req) { return Response::plainText(200, std::string(methodToString(req.method))); });

	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/multi", Method::GET)).body, "GET");
	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/multi", Method::PUT)).body, "PUT");
	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/multi", Method::POST)).statusCode, 404);
}

// --- Static file serving ---

BOOST_AUTO_TEST_CASE(test_static_file_serving_and_mime_types) {
	const TempDir root("static");
	root.write("page.html", "<h1>hi</h1>");
	root.write("notes.txt", "plain");
	root.write("blob.bin", "\x01\x02");

	TestServer server(sConfig(root.path()));

	const Response html = server.handleStaticFileRequest(sMakeRequest("/page.html"));
	BOOST_CHECK_EQUAL(html.statusCode, 200);
	BOOST_CHECK_EQUAL(html.body, "<h1>hi</h1>");
	BOOST_CHECK_EQUAL(html.headers.at("Content-Type"), "text/html");

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeRequest("/notes.txt")).headers.at("Content-Type"), "text/plain");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeRequest("/blob.bin")).headers.at("Content-Type"), "application/octet-stream");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeRequest("/missing.txt")).statusCode, 404);
}

BOOST_AUTO_TEST_CASE(test_directory_traversal_is_rejected) {
	const TempDir outer("traversal");
	outer.write("secret.txt", "secret");
	std::filesystem::create_directory(outer.path() / "www");
	outer.write("www/public.txt", "public");

	TestServer server(sConfig(outer.path() / "www"));

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeRequest("/public.txt")).statusCode, 200);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeRequest("/../secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeRequest("/../../etc/passwd")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sMakeRequest("/subdir/../../etc/passwd")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(sParse("GET /..%2Fsecret.txt HTTP/1.1\r\n\r\n")).statusCode, 403);
}

BOOST_AUTO_TEST_CASE(test_directory_indexing_escapes_names) {
	const TempDir root("indexing");
	root.write("a<script>.txt", "x");
	root.write("a b.txt", "x");

	TestServer serverOff(sConfig(root.path()));
	BOOST_CHECK_EQUAL(serverOff.handleStaticFileRequest(sMakeRequest("/")).statusCode, 403);

	Server::Config indexing = sConfig(root.path());
	indexing.enableDirectoryIndexing = true;
	TestServer serverOn(indexing);
	const Response index = serverOn.handleStaticFileRequest(sMakeRequest("/"));
	BOOST_CHECK_EQUAL(index.statusCode, 200);
	BOOST_CHECK(sContains(index.body, "&lt;script&gt;"));
	BOOST_CHECK(!sContains(index.body, "<script>"));
	BOOST_CHECK(sContains(index.body, "href=\"/a%20b.txt\""));
	BOOST_CHECK(sContains(index.body, ">a b.txt<"));
}

// --- Key-value store ---

BOOST_AUTO_TEST_CASE(test_kv_store_uses_decoded_query) {
	KVStore store;

	Request put = sMakeRequest("/kv", Method::PUT);
	put.query = "key=my%20key";
	put.body = "value";
	BOOST_CHECK_EQUAL(store.handle(put).statusCode, 200);
	BOOST_CHECK_EQUAL(store.size(), 1U);
	BOOST_CHECK(store.get("my key") == std::optional<std::string>("value"));

	Request get = sMakeRequest("/kv");
	get.query = "key=my+key";
	BOOST_CHECK_EQUAL(store.handle(get).body, "value");

	BOOST_CHECK_EQUAL(store.handle(sMakeRequest("/kv")).statusCode, 400);

	Request remove = sMakeRequest("/kv", Method::DELETE);
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

// --- Access log ---

BOOST_AUTO_TEST_CASE(test_access_log_middleware) {
	const TempDir dir("access");
	const std::filesystem::path logPath = dir.path() / "access.log";
	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 0 });

	Request request = sMakeRequest("/logged");
	Response response = Response::plainText(200, "hello");

	BOOST_CHECK(!accessLog.process(request, response));
	accessLog.postProcess(request, response);

	const std::vector<std::string> lines = sLines(logPath);
	BOOST_REQUIRE_EQUAL(lines.size(), 1U);
	BOOST_CHECK(sContains(lines[0], "127.0.0.1"));
	BOOST_CHECK(sContains(lines[0], "GET /logged"));
	BOOST_CHECK(sContains(lines[0], " 200 5"));

	// A request with no client address is not logged
	Request anonymous;
	anonymous.path = "/anonymous";
	accessLog.postProcess(anonymous, response);
	BOOST_CHECK_EQUAL(sLines(logPath).size(), 1U);
}

BOOST_AUTO_TEST_CASE(test_access_log_buffers_until_flushed) {
	const TempDir dir("buffered");
	const std::filesystem::path logPath = dir.path() / "access.log";
	const Response response = Response::plainText(200, "hello");
	Request request = sMakeRequest("/buffered");

	{
		AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = size_t{ 64 } * 1024, .flushInterval = std::chrono::hours(1) });

		for (int i = 0; i < 10; ++i)
			accessLog.postProcess(request, response);
		BOOST_CHECK(!std::filesystem::exists(logPath));

		accessLog.flush();
		BOOST_CHECK_EQUAL(sLines(logPath).size(), 10U);

		std::filesystem::remove(logPath);
		accessLog.postProcess(request, response);
		accessLog.flush();
		BOOST_CHECK_EQUAL(sLines(logPath).size(), 1U);

		std::filesystem::remove(logPath);
		accessLog.postProcess(request, response);
		BOOST_CHECK(!std::filesystem::exists(logPath));
	}

	BOOST_CHECK_EQUAL(sLines(logPath).size(), 1U);
}

BOOST_AUTO_TEST_CASE(test_access_log_flushes_when_buffer_fills) {
	const TempDir dir("bufferfull");
	const std::filesystem::path logPath = dir.path() / "access.log";

	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 16, .flushInterval = std::chrono::hours(1) });
	Request request = sMakeRequest("/full");
	const Response response = Response::plainText(200, "hello");

	for (int i = 0; i < 3; ++i)
		accessLog.postProcess(request, response);

	BOOST_CHECK_EQUAL(sLines(logPath).size(), 3U);
}

BOOST_AUTO_TEST_CASE(test_access_log_concurrent_writes) {
	const TempDir dir("concurrent");
	const std::filesystem::path logPath = dir.path() / "access.log";
	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 0 });
	const Response response = Response::plainText(200, "hello");

	constexpr int kThreadCount = 4;
	constexpr int kPerThread = 25;
	std::vector<std::thread> threads;
	threads.reserve(kThreadCount);
	for (int i = 0; i < kThreadCount; ++i) {
		threads.emplace_back([&accessLog, &response]() {
			Request request = sMakeRequest("/concurrent");
			for (int j = 0; j < kPerThread; ++j)
				accessLog.postProcess(request, response);
		});
	}
	for (std::thread &thread : threads)
		thread.join();

	BOOST_CHECK_EQUAL(sLines(logPath).size(), static_cast<size_t>(kThreadCount * kPerThread));
}

BOOST_AUTO_TEST_CASE(test_access_log_records_routed_status) {
	const TempDir dir("routed_status");
	const std::filesystem::path logPath = dir.path() / "access.log";

	TestServer server(sConfig("."));
	server.addMiddleware(std::make_shared<AccessLog>(AccessLog::Config{ .path = logPath, .flushBytes = 0 }));
	server.registerPathHandler({ Method::GET, Method::HEAD }, "/hello", [](const Request &) { return Response::plainText(200, "hi there"); });

	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/hello")).body, "hi there");
	BOOST_CHECK_EQUAL(server.handleRequest(sMakeRequest("/does-not-exist")).statusCode, 404);
	BOOST_CHECK(server.handleRequest(sMakeRequest("/hello", Method::HEAD)).body.empty());

	const std::vector<std::string> lines = sLines(logPath);
	BOOST_REQUIRE_EQUAL(lines.size(), 3U);
	BOOST_CHECK(sContains(lines[0], "GET /hello"));
	BOOST_CHECK(sContains(lines[0], " 200 8"));
	BOOST_CHECK(sContains(lines[1], "GET /does-not-exist"));
	BOOST_CHECK(sContains(lines[1], " 404 "));
	BOOST_CHECK(sContains(lines[2], "HEAD /hello"));
	BOOST_CHECK(sContains(lines[2], " 200 8"));
}

// --- Live socket tests ---

namespace {

ou::UniqueFd sConnect(uint16_t port) {
	ou::UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
	BOOST_REQUIRE(fd.valid());

	// So a regression hangs this case rather than the whole ctest timeout
	const timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
	(void)::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	BOOST_REQUIRE_EQUAL(::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
	return fd;
}

void sSend(const ou::UniqueFd &fd, std::string_view bytes) {
	BOOST_REQUIRE_EQUAL(::send(fd.get(), bytes.data(), bytes.size(), 0), ssize_t(bytes.size()));
}

void sCheckClosed(const ou::UniqueFd &fd) {
	std::array<char, 64> chunk{};
	BOOST_CHECK_EQUAL(::recv(fd.get(), chunk.data(), chunk.size(), 0), 0);
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

std::string sReadResponse(const ou::UniqueFd &fd) {
	return sReadResponse([&fd](std::span<char> buffer) { return ::recv(fd.get(), buffer.data(), buffer.size(), 0); });
}

std::string sRoundTrip(uint16_t port, std::initializer_list<std::string_view> pieces) {
	const ou::UniqueFd fd = sConnect(port);

	for (const std::string_view piece : pieces) {
		sSend(fd, piece);
		if (pieces.size() > 1)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	return sReadResponse(fd);
}

std::string sRoundTrip(uint16_t port, std::string_view rawRequest) { return sRoundTrip(port, { rawRequest }); }

} // namespace

BOOST_DATA_TEST_CASE(test_server_round_trip, bdata::make(kModels), model) {
	const TempDir root("socket");
	root.write("hello.txt", "hello socket");
	root.write("large.txt", std::string(200000, 'x'));

	Server server(sConfig(root.path(), model));
	BOOST_REQUIRE(server.start());
	const uint16_t port = server.port();

	const std::string ok = sRoundTrip(port, "GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(ok.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(ok.ends_with("hello socket"));

	const std::string head = sRoundTrip(port, "HEAD /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(head.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK(head.ends_with("\r\n\r\n"));

	const std::string split = sRoundTrip(port, { "GET /hello.txt HTTP/1.1\r\nHost: ", "x\r\n", "\r\n" });
	BOOST_CHECK(split.ends_with("hello socket"));

	const std::string large = sRoundTrip(port, "GET /large.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(large.starts_with("HTTP/1.1 200 OK"));
	BOOST_CHECK_EQUAL(large.size() - large.find("\r\n\r\n") - 4, 200000U);

	BOOST_CHECK(sRoundTrip(port, "GET /nope.txt HTTP/1.1\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 404"));
	BOOST_CHECK(sRoundTrip(port, "BOGUS / HTTP/1.1\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 400"));
	BOOST_CHECK(sRoundTrip(port, "GET /hello.txt HTTP/2.0\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 400"));
	BOOST_CHECK(
			sRoundTrip(port, "POST /hello.txt HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n").starts_with("HTTP/1.1 501"));

	// Must return rather than deadlock, and must tolerate the second call ~Server makes
	server.stop();
	server.stop();
}

BOOST_DATA_TEST_CASE(test_keep_alive, bdata::make(kModels), model) {
	const TempDir root("keepalive");
	root.write("a.txt", "aaa");
	root.write("b.txt", "bbbb");

	Server server(sConfig(root.path(), model));
	BOOST_REQUIRE(server.start());
	const uint16_t port = server.port();

	{ // Two requests down one connection
		const ou::UniqueFd fd = sConnect(port);
		sSend(fd, "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
		const std::string first = sReadResponse(fd);
		BOOST_CHECK(first.starts_with("HTTP/1.1 200 OK"));
		BOOST_CHECK(first.ends_with("aaa"));

		sSend(fd, "GET /b.txt HTTP/1.1\r\nHost: x\r\n\r\n");
		BOOST_CHECK(sReadResponse(fd).ends_with("bbbb"));
	}

	{ // Pipelined: both requests arrive in one write, both get answered
		const ou::UniqueFd fd = sConnect(port);
		sSend(fd, "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /b.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
		BOOST_CHECK(sReadResponse(fd).ends_with("aaa"));
		BOOST_CHECK(sReadResponse(fd).ends_with("bbbb"));
	}

	{ // Connection: close is honoured, and the server really closes
		const ou::UniqueFd fd = sConnect(port);
		sSend(fd, "GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
		BOOST_CHECK(sContains(sReadResponse(fd), "Connection: close"));
		sCheckClosed(fd);
	}

	{ // HTTP/1.0 closes unless it asks otherwise
		const ou::UniqueFd fd = sConnect(port);
		sSend(fd, "GET /a.txt HTTP/1.0\r\nHost: x\r\n\r\n");
		BOOST_CHECK(sContains(sReadResponse(fd), "Connection: close"));
		sCheckClosed(fd);
	}
}

BOOST_DATA_TEST_CASE(test_keep_alive_can_be_disabled, bdata::make(kModels), model) {
	const TempDir root("keepalive-off");
	root.write("a.txt", "aaa");

	Server::Config config = sConfig(root.path(), model);
	config.keepAlive = false;
	Server server(config);
	BOOST_REQUIRE(server.start());

	const ou::UniqueFd fd = sConnect(server.port());
	sSend(fd, "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	BOOST_CHECK(sContains(sReadResponse(fd), "Connection: close"));
	sCheckClosed(fd);
}

BOOST_DATA_TEST_CASE(test_rejection_is_logged, bdata::make(kModels), model) {
	const TempDir root("rejectlog");
	const std::filesystem::path logPath = root.path() / "access.log";

	Server server(sConfig(root.path(), model));
	server.addMiddleware(std::make_shared<AccessLog>(AccessLog::Config{ .path = logPath, .flushBytes = 0 }));
	BOOST_REQUIRE(server.start());

	BOOST_CHECK(sRoundTrip(server.port(), "BOGUS / HTTP/1.1\r\nHost: x\r\n\r\n").starts_with("HTTP/1.1 400"));
	server.stop();

	const std::vector<std::string> lines = sLines(logPath);
	BOOST_REQUIRE_EQUAL(lines.size(), 1U);
	BOOST_CHECK(sContains(lines[0], " 400 "));
	BOOST_CHECK(sContains(lines[0], "\"-\""));
}

BOOST_AUTO_TEST_CASE(test_event_loop_closes_idle_connection) {
	const TempDir root("eventloop-idle");

	Server::Config config = sConfig(root.path(), Server::Model::EventLoop);
	config.readTimeout = std::chrono::seconds(1);
	Server server(config);
	BOOST_REQUIRE(server.start());

	const ou::UniqueFd fd = sConnect(server.port());
	sCheckClosed(fd);
}

#ifndef DISABLE_HTTPS

namespace {

void sCloseTls(SSL *ssl) {
	SSL_shutdown(ssl);
	SSL_free(ssl);
}

// A TLS client that does not verify the peer, since the demo cert is self-signed
struct TlsClient {
	std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx{ SSL_CTX_new(TLS_client_method()), &SSL_CTX_free };
	ou::UniqueFd fd;
	std::unique_ptr<SSL, decltype(&sCloseTls)> ssl{ SSL_new(ctx.get()), &sCloseTls };

	explicit TlsClient(uint16_t port) : fd(sConnect(port)) {
		SSL_set_fd(ssl.get(), fd.get());
		BOOST_REQUIRE_EQUAL(SSL_connect(ssl.get()), 1);
	}

	[[nodiscard]] bool send(std::string_view request) const {
		return SSL_write(ssl.get(), request.data(), static_cast<int>(request.size())) > 0;
	}

	[[nodiscard]] std::string receive() const {
		return sReadResponse([this](std::span<char> buffer) { return SSL_read(ssl.get(), buffer.data(), static_cast<int>(buffer.size())); });
	}
};

} // namespace

BOOST_DATA_TEST_CASE(test_tls, bdata::make(kModels), model) {
	const TempDir root("tls");
	root.write("a.txt", "over tls");

	Server::Config config = sConfig(root.path(), model);
	config.https = { .enabled = true,
									 .certPath = std::filesystem::path(TEST_CERT_DIR) / "cert.pem",
									 .keyPath = std::filesystem::path(TEST_CERT_DIR) / "key.pem" };
	Server server(config);
	BOOST_REQUIRE(server.start());
	const uint16_t port = server.port();

	{ // Keep-alive over TLS: two requests on one session
		const TlsClient client(port);
		BOOST_REQUIRE(client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
		const std::string response = client.receive();
		BOOST_CHECK(response.starts_with("HTTP/1.1 200 OK"));
		BOOST_CHECK(response.ends_with("over tls"));

		BOOST_REQUIRE(client.send("GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
		BOOST_CHECK(client.receive().starts_with("HTTP/1.1 404"));
	}

	if (model != Server::Model::EventLoop)
		return;

	std::vector<TlsClient> clients;
	clients.reserve(8);
	for (int i = 0; i < 8; ++i)
		clients.emplace_back(port);

	for (const TlsClient &client : clients)
		BOOST_REQUIRE(client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
	for (const TlsClient &client : clients)
		BOOST_CHECK(client.receive().ends_with("over tls"));
}

#endif // DISABLE_HTTPS
