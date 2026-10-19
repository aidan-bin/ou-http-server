#include "Awaitables.h"
#include "Extractors.h"
#include "HttpTypes.h"
#include "KVStore.h"
#include "TestSupport.h"

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace ou::http;
using namespace ou::test;

static_assert(Extractor<Query<"key">>);

namespace {

class TestMiddleware : public Middleware {
public:
	std::optional<Response> process(Request &request) override {
		if (request.path == "/middleware")
			return Response::plainText(200, "Intercepted by Middleware");
		return std::nullopt;
	}
};

struct RoutingFixture {
	TestServer server{ makeConfig(".") };
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(routing, RoutingFixture)

BOOST_AUTO_TEST_CASE(test_custom_handler_and_middleware) {
	server.addMiddleware(std::make_shared<TestMiddleware>());
	server.registerPathHandler(Method::GET, "/test", [](const Request &) { return Response::plainText(200, "Handled by TestHandler"); });
	server.registerPatternHandler(Method::GET, R"(/pattern/\d+)",
																[](const Request &) { return Response::plainText(200, "Pattern handler"); });

	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/test")).body, "Handled by TestHandler");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/middleware")).body, "Intercepted by Middleware");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/pattern/123")).body, "Pattern handler");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/unknown")).statusCode, 404);
}

BOOST_AUTO_TEST_CASE(test_coroutine_handler_routes_like_any_other) {
	server.addMiddleware(std::make_shared<TestMiddleware>());
	const auto napping = [](const Request &) -> ou::Task<Response> {
		co_await sleepFor(std::chrono::milliseconds(1));
		co_return Response::plainText(200, "rested");
	};
	server.registerPathHandler({ Method::GET, Method::HEAD }, "/nap", napping);
	server.registerPatternHandler(Method::GET, R"(/nap/\d+)", napping);
	server.registerPathHandler(Method::GET, "/middleware", napping);

	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/nap")).body, "rested");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/nap/7")).body, "rested");
	BOOST_CHECK(server.handleRequest(makeRequest("/nap", Method::HEAD)).body.empty());
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/middleware")).body, "Intercepted by Middleware");
}

BOOST_AUTO_TEST_CASE(test_multi_method_registration) {
	const auto echoMethod = [](const Request &req) {
		return Response::plainText(200, std::string(methodToString(req.method)));
	};
	server.registerPathHandler({ Method::GET, Method::PUT }, "/multi", echoMethod);
	server.registerPatternHandler({ Method::GET, Method::DELETE }, R"(/multi/\d+)", echoMethod);

	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/multi", Method::GET)).body, "GET");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/multi", Method::PUT)).body, "PUT");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/multi", Method::POST)).statusCode, 404);
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/multi/1", Method::DELETE)).body, "DELETE");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/multi/1", Method::PUT)).statusCode, 404);
}

BOOST_AUTO_TEST_CASE(test_query_extractor) {
	Request request = makeRequest("/");
	request.query = "name=my%20name&n=42&bad=4x&empty=";

	BOOST_CHECK_EQUAL(Query<"name">::extract(request)->value, "my name");
	BOOST_CHECK_EQUAL((Query<"n", int>::extract(request)->value), 42);

	const auto bad = Query<"bad", int>::extract(request);
	BOOST_REQUIRE(!bad);
	BOOST_CHECK_EQUAL(bad.error().statusCode, 400);
	BOOST_CHECK_EQUAL(bad.error().body, "Missing or bad bad parameter");
	BOOST_CHECK(!Query<"empty">::extract(request));
	BOOST_CHECK(!Query<"missing">::extract(request));
}

BOOST_AUTO_TEST_CASE(test_pattern_captures) {
	server.registerPatternHandler(Method::GET, R"(/users/(\w+)/posts/(\d+)?)", [](const Request &request) {
		return Response::plainText(200, std::format("{}|{}|{}", request.pathParam(1).value_or("-"), request.pathParam(2).value_or("-"),
																								request.pathParam(3).value_or("-")));
	});
	server.registerPatternHandler(Method::GET, R"(/id/(\w+))",
																[](Path<1, int> id) { return Response::plainText(200, std::to_string(id.value)); });

	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/users/ann/posts/7")).body, "ann|7|-");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/users/ann/posts/")).body, "ann||-"); // Unmatched group is empty
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/id/42")).body, "42");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/id/abc")).body, "Bad path parameter 1");
}

BOOST_AUTO_TEST_CASE(test_named_segments) {
	server.registerPatternHandler(Method::GET, "/users/{user}/posts/{id}.json", [](Path<"user"> user, Path<"id", int> id) {
		return Response::plainText(200, std::format("{} {}", user.value, id.value));
	});
	server.registerPatternHandler(Method::GET, "/raw/{n}",
																[](Path<1> n, Path<"missing"> missing) { return Response::plainText(200, n.value + missing.value); });

	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/users/ann/posts/7.json")).body, "ann 7");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/users/ann/posts/x.json")).body, "Bad path parameter id");
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/users/ann/posts/7xjson")).statusCode, 404); // "." is literal
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/users/a/b/posts/7.json")).statusCode, 404); // A segment has no "/"
	BOOST_CHECK_EQUAL(server.handleRequest(makeRequest("/raw/1")).body, "Bad path parameter missing");
}

BOOST_AUTO_TEST_CASE(test_extracting_handler) {
	server.registerPathHandler(Method::GET, "/add", [](Query<"a", int> a, Query<"b", int> b) {
		return Response::plainText(200, std::to_string(a.value + b.value));
	});
	server.registerPathHandler({ Method::GET, Method::PUT }, "/echo", [](const Request &request, Query<"name"> name) -> ou::Task<Response> {
		co_await sleepFor(std::chrono::milliseconds(1));
		co_return Response::plainText(200, std::format("{} {}", request.method, name.value));
	});

	const auto call = [&](std::string_view path, std::string_view query, Method method = Method::GET) {
		Request request = makeRequest(std::string(path), method);
		request.query = query;
		return server.handleRequest(request);
	};

	BOOST_CHECK_EQUAL(call("/add", "a=2&b=3").body, "5");
	BOOST_CHECK_EQUAL(call("/add", "a=x&b=y").body, "Missing or bad a parameter"); // First failure wins
	BOOST_CHECK_EQUAL(call("/add", "a=2").statusCode, 400);
	BOOST_CHECK_EQUAL(call("/echo", "name=ou", Method::PUT).body, "PUT ou");
	BOOST_CHECK_EQUAL(call("/echo", "").statusCode, 400);

	server.registerPatternHandler(Method::GET, R"(/scaled/(\d+))", [](Path<1, int> n, Query<"by", int> by) {
		return Response::plainText(200, std::to_string(n.value * by.value));
	});
	BOOST_CHECK_EQUAL(call("/scaled/3", "by=4").body, "12");
	BOOST_CHECK_EQUAL(call("/scaled/3", "").statusCode, 400);

	server.registerPathHandler(Method::GET, "/agent", [](Header<"user-agent"> agent, Header<"X-Count", int> count) {
		return Response::plainText(200, std::format("{} {}", agent.value, count.value));
	});
	Request request = makeRequest("/agent");
	request.headers = { { "User-Agent", "ou" }, { "X-Count", "3" } };
	BOOST_CHECK_EQUAL(server.handleRequest(request).body, "ou 3");
	request.headers.pop_back();
	BOOST_CHECK_EQUAL(server.handleRequest(request).body, "Missing or bad X-Count header");
}

BOOST_AUTO_TEST_CASE(test_kv_store_methods) {
	KVStore store;

	Request put = makeRequest("/kv", Method::PUT);
	put.body = "value";
	BOOST_CHECK_EQUAL(store.handle(put, "my key").statusCode, 200);
	BOOST_CHECK_EQUAL(store.size(), 1U);
	BOOST_CHECK(store.get("my key") == std::optional<std::string>("value"));

	BOOST_CHECK_EQUAL(store.handle(makeRequest("/kv"), "my key").body, "value");
	BOOST_CHECK_EQUAL(store.handle(makeRequest("/kv"), "other").statusCode, 404);
	BOOST_CHECK_EQUAL(store.handle(makeRequest("/kv", Method::POST), "my key").statusCode, 405);

	const Request remove = makeRequest("/kv", Method::DELETE);
	BOOST_CHECK_EQUAL(store.handle(remove, "my key").statusCode, 200);
	BOOST_CHECK_EQUAL(store.size(), 0U);
	BOOST_CHECK_EQUAL(store.handle(remove, "my key").statusCode, 404);
}

BOOST_AUTO_TEST_SUITE_END()
