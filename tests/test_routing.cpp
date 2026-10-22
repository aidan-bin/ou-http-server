#include "KVStore.h"
#include "TestSupport.h"
#include "ou/http/Awaitables.h"
#include "ou/http/Extractors.h"
#include "ou/http/HttpTypes.h"

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace ou::http;
using namespace ou::test;

static_assert(Extractor<Query<"key">>);
static_assert(IntoResponse<Response> && IntoResponse<std::string> && IntoResponse<const char *>);
static_assert(IntoResponse<std::expected<std::string, Response>>);
static_assert(!IntoResponse<int> && !IntoResponse<std::expected<int, Response>>);

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
	Router router;
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(routing, RoutingFixture)

BOOST_AUTO_TEST_CASE(test_custom_handler_and_middleware) {
	router.layer(std::make_shared<TestMiddleware>());
	router.route(Method::GET, "/test", [](const Request &) { return Response::plainText(200, "Handled by TestHandler"); });
	router.pattern(Method::GET, R"(/pattern/\d+)", [](const Request &) { return Response::plainText(200, "Pattern handler"); });

	BOOST_CHECK_EQUAL(respond(router, makeRequest("/test")).body, "Handled by TestHandler");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/middleware")).body, "Intercepted by Middleware");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/pattern/123")).body, "Pattern handler");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/unknown")).statusCode, 404);
}

BOOST_AUTO_TEST_CASE(test_fallback_answers_unmatched_requests) {
	router.route(Method::GET, "/known", [](const Request &) { return Response::plainText(200, "known"); })
			.fallback([](const Request &request) { return Response::plainText(404, "no " + request.path); });

	BOOST_CHECK_EQUAL(respond(router, makeRequest("/known")).body, "known");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/other")).body, "no /other");
}

BOOST_AUTO_TEST_CASE(test_coroutine_handler_routes_like_any_other) {
	router.layer(std::make_shared<TestMiddleware>());
	const auto napping = [](const Request &) -> Task<Response> {
		co_await sleepFor(std::chrono::milliseconds(1));
		co_return Response::plainText(200, "rested");
	};
	router.route({ Method::GET, Method::HEAD }, "/nap", napping);
	router.pattern(Method::GET, R"(/nap/\d+)", napping);
	router.route(Method::GET, "/middleware", napping);

	BOOST_CHECK_EQUAL(respond(router, makeRequest("/nap")).body, "rested");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/nap/7")).body, "rested");
	BOOST_CHECK(respond(router, makeRequest("/nap", Method::HEAD)).body.empty());
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/middleware")).body, "Intercepted by Middleware");
}

BOOST_AUTO_TEST_CASE(test_multi_method_registration) {
	const auto echoMethod = [](const Request &req) {
		return Response::plainText(200, std::string(methodToString(req.method)));
	};
	router.route({ Method::GET, Method::PUT }, "/multi", echoMethod);
	router.pattern({ Method::GET, Method::DELETE }, R"(/multi/\d+)", echoMethod);

	BOOST_CHECK_EQUAL(respond(router, makeRequest("/multi", Method::GET)).body, "GET");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/multi", Method::PUT)).body, "PUT");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/multi", Method::POST)).statusCode, 404);
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/multi/1", Method::DELETE)).body, "DELETE");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/multi/1", Method::PUT)).statusCode, 404);
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
	router.pattern(Method::GET, R"(/users/(\w+)/posts/(\d+)?)", [](const Request &request) {
		return Response::plainText(200, std::format("{}|{}|{}", request.pathParam(1).value_or("-"), request.pathParam(2).value_or("-"),
																								request.pathParam(3).value_or("-")));
	});
	router.pattern(Method::GET, R"(/id/(\w+))", [](Path<1, int> id) { return Response::plainText(200, std::to_string(id.value)); });

	BOOST_CHECK_EQUAL(respond(router, makeRequest("/users/ann/posts/7")).body, "ann|7|-");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/users/ann/posts/")).body, "ann||-"); // Unmatched group is empty
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/id/42")).body, "42");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/id/abc")).body, "Bad path parameter 1");
}

BOOST_AUTO_TEST_CASE(test_named_segments) {
	router.pattern(Method::GET, "/users/{user}/posts/{id}.json", [](Path<"user"> user, Path<"id", int> id) {
		return Response::plainText(200, std::format("{} {}", user.value, id.value));
	});
	router.pattern(Method::GET, "/raw/{n}",
								 [](Path<1> n, Path<"missing"> missing) { return Response::plainText(200, n.value + missing.value); });

	BOOST_CHECK_EQUAL(respond(router, makeRequest("/users/ann/posts/7.json")).body, "ann 7");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/users/ann/posts/x.json")).body, "Bad path parameter id");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/users/ann/posts/7xjson")).statusCode, 404); // "." is literal
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/users/a/b/posts/7.json")).statusCode, 404); // A segment has no "/"
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/raw/1")).body, "Bad path parameter missing");
}

BOOST_AUTO_TEST_CASE(test_extracting_handler) {
	router.route(Method::GET, "/add",
							 [](Query<"a", int> a, Query<"b", int> b) { return Response::plainText(200, std::to_string(a.value + b.value)); });
	router.route({ Method::GET, Method::PUT }, "/echo", [](const Request &request, Query<"name"> name) -> Task<Response> {
		co_await sleepFor(std::chrono::milliseconds(1));
		co_return Response::plainText(200, std::format("{} {}", request.method, name.value));
	});

	const auto call = [&](std::string_view path, std::string_view query, Method method = Method::GET) {
		Request request = makeRequest(std::string(path), method);
		request.query = query;
		return respond(router, request);
	};

	BOOST_CHECK_EQUAL(call("/add", "a=2&b=3").body, "5");
	BOOST_CHECK_EQUAL(call("/add", "a=x&b=y").body, "Missing or bad a parameter"); // First failure wins
	BOOST_CHECK_EQUAL(call("/add", "a=2").statusCode, 400);
	BOOST_CHECK_EQUAL(call("/echo", "name=ou", Method::PUT).body, "PUT ou");
	BOOST_CHECK_EQUAL(call("/echo", "").statusCode, 400);

	router.pattern(Method::GET, R"(/scaled/(\d+))",
								 [](Path<1, int> n, Query<"by", int> by) { return Response::plainText(200, std::to_string(n.value * by.value)); });
	BOOST_CHECK_EQUAL(call("/scaled/3", "by=4").body, "12");
	BOOST_CHECK_EQUAL(call("/scaled/3", "").statusCode, 400);

	router.route(Method::GET, "/agent", [](Header<"user-agent"> agent, Header<"X-Count", int> count) {
		return Response::plainText(200, std::format("{} {}", agent.value, count.value));
	});
	Request request = makeRequest("/agent");
	request.headers = { { "User-Agent", "ou" }, { "X-Count", "3" } };
	BOOST_CHECK_EQUAL(respond(router, request).body, "ou 3");
	request.headers.pop_back();
	BOOST_CHECK_EQUAL(respond(router, request).body, "Missing or bad X-Count header");
}

BOOST_AUTO_TEST_CASE(test_handler_returns_anything_into_response) {
	router.route(Method::GET, "/text", [](Query<"name"> name) { return "hi " + name.value; });
	router.route(Method::GET, "/result", [](Query<"n", int> n) -> std::expected<std::string, Response> {
		if (n.value < 0)
			return std::unexpected(Response::error(422));
		return std::to_string(n.value);
	});
	router.route(Method::GET, "/later", [](Query<"name"> name) -> Task<std::string> {
		co_await sleepFor(std::chrono::milliseconds(1));
		co_return "hi " + name.value;
	});

	const auto call = [&](std::string_view path, std::string_view query) {
		Request request = makeRequest(std::string(path));
		request.query = query;
		return respond(router, request);
	};

	const Response text = call("/text", "name=ou");
	BOOST_CHECK_EQUAL(text.statusCode, 200);
	BOOST_CHECK_EQUAL(text.body, "hi ou");
	BOOST_CHECK_EQUAL(call("/text", "").statusCode, 400);
	BOOST_CHECK_EQUAL(call("/result", "n=7").body, "7");
	BOOST_CHECK_EQUAL(call("/result", "n=-1").statusCode, 422);
	BOOST_CHECK_EQUAL(call("/later", "name=ou").body, "hi ou");
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
