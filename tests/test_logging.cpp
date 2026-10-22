#include "ou/http/Logging.h"

#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>

BOOST_AUTO_TEST_SUITE(logging)

BOOST_AUTO_TEST_CASE(test_sink_receives_formatted_lines) {
	std::vector<std::string> lines;
	ou::setLogSink([&lines](std::string_view line) { lines.emplace_back(line); });

	ou::logInfo("started {}", 1);
	ou::logWarn("careful");
	ou::setLogSink({});

	BOOST_REQUIRE_EQUAL(lines.size(), 2U);
	BOOST_CHECK_EQUAL(lines[0], "[INFO] started 1");
	BOOST_CHECK(lines[1].starts_with("[WARNING] test_logging.cpp:"));
	BOOST_CHECK(lines[1].ends_with(": careful"));
}

BOOST_AUTO_TEST_SUITE_END()
