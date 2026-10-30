#include "TestSupport.h"
#include "ou/http/AccessLog.h"
#include "ou/http/Request.h"
#include "ou/http/Response.h"

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ou::http;
using namespace ou::test;

namespace {

struct AccessLogFixture {
	TempDir dir;
	std::filesystem::path logPath = dir.path() / "access.log";
	Request request = makeRequest("/logged");
	Response response = Response::plainText(200, "hello");
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(access_log, AccessLogFixture)

BOOST_AUTO_TEST_CASE(test_access_log_middleware) {
	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 0 });

	BOOST_CHECK(!accessLog.before(request));
	accessLog.after(request, response);

	const std::vector<std::string> logged = lines(logPath);
	BOOST_REQUIRE_EQUAL(logged.size(), 1U);
	BOOST_CHECK(logged[0].contains("127.0.0.1"));
	BOOST_CHECK(logged[0].contains("GET /logged"));
	BOOST_CHECK(logged[0].contains(" 200 5"));

	// A request with no client address is not logged
	Request anonymous;
	anonymous.path = "/anonymous";
	accessLog.after(anonymous, response);
	BOOST_CHECK_EQUAL(lines(logPath).size(), 1U);
}

BOOST_AUTO_TEST_CASE(test_access_log_buffers_until_flushed) {
	{
		AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = size_t{ 64 } * 1024, .flushInterval = std::chrono::hours(1) });

		for (int i = 0; i < 10; ++i)
			accessLog.after(request, response);
		BOOST_CHECK(!std::filesystem::exists(logPath));

		accessLog.flush();
		BOOST_CHECK_EQUAL(lines(logPath).size(), 10U);

		std::filesystem::remove(logPath);
		accessLog.after(request, response);
		accessLog.flush();
		BOOST_CHECK_EQUAL(lines(logPath).size(), 1U);

		std::filesystem::remove(logPath);
		accessLog.after(request, response);
		BOOST_CHECK(!std::filesystem::exists(logPath));
	}

	BOOST_CHECK_EQUAL(lines(logPath).size(), 1U);
}

BOOST_AUTO_TEST_CASE(test_access_log_flushes_when_buffer_fills) {
	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 16, .flushInterval = std::chrono::hours(1) });

	for (int i = 0; i < 3; ++i)
		accessLog.after(request, response);

	BOOST_CHECK_EQUAL(lines(logPath).size(), 3U);
}

BOOST_AUTO_TEST_CASE(test_access_log_concurrent_writes) {
	AccessLog accessLog(AccessLog::Config{ .path = logPath, .flushBytes = 0 });

	constexpr int kThreadCount = 4;
	constexpr int kPerThread = 25;
	std::vector<std::jthread> threads;
	threads.reserve(kThreadCount);
	for (int i = 0; i < kThreadCount; ++i) {
		threads.emplace_back([this, &accessLog]() {
			for (int j = 0; j < kPerThread; ++j)
				accessLog.after(request, response);
		});
	}
	threads.clear();

	BOOST_CHECK_EQUAL(lines(logPath).size(), static_cast<size_t>(kThreadCount * kPerThread));
}

BOOST_AUTO_TEST_CASE(test_access_log_records_routed_status) {
	Router router;
	router.use(std::make_shared<AccessLog>(AccessLog::Config{ .path = logPath, .flushBytes = 0 }));
	const auto hello = [](const Request &) {
		return Response::plainText(200, "hi there");
	};
	router.route("/hello", get(hello), head(hello));

	BOOST_CHECK_EQUAL(respond(router, makeRequest("/hello")).body, "hi there");
	BOOST_CHECK_EQUAL(respond(router, makeRequest("/does-not-exist")).statusCode, 404);
	BOOST_CHECK(respond(router, makeRequest("/hello", Method::HEAD)).body.empty());

	const std::vector<std::string> logged = lines(logPath);
	BOOST_REQUIRE_EQUAL(logged.size(), 3U);
	BOOST_CHECK(logged[0].contains("GET /hello"));
	BOOST_CHECK(logged[0].contains(" 200 8"));
	BOOST_CHECK(logged[1].contains("GET /does-not-exist"));
	BOOST_CHECK(logged[1].contains(" 404 "));
	BOOST_CHECK(logged[2].contains("HEAD /hello"));
	BOOST_CHECK(logged[2].contains(" 200 8"));
}

BOOST_AUTO_TEST_SUITE_END()
