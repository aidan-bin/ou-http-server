#include "TestSupport.h"
#include "ou/http/HttpTypes.h"
#include "ou/http/StaticFiles.h"

#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <string>

using namespace ou::http;
using namespace ou::test;

BOOST_FIXTURE_TEST_SUITE(static_files, ServingRoot)

BOOST_AUTO_TEST_CASE(test_static_file_serving_and_mime_types) {
	root.write("page.html", "<h1>hi</h1>");
	root.write("notes.txt", "plain");
	root.write("blob.bin", "\x01\x02");
	root.write("UPPER.TXT", "loud");
	root.write(".html", "dotfile");

	const StaticFiles files = openFiles({ .directory = root.path() });

	const Response html = files(makeRequest("/page.html"));
	BOOST_CHECK_EQUAL(html.statusCode, 200);
	BOOST_CHECK_EQUAL(html.body, "<h1>hi</h1>");
	BOOST_CHECK_EQUAL(html.headers.at("Content-Type"), "text/html");

	BOOST_CHECK_EQUAL(files(makeRequest("/notes.txt")).headers.at("Content-Type"), "text/plain");
	BOOST_CHECK_EQUAL(files(makeRequest("/blob.bin")).headers.at("Content-Type"), "application/octet-stream");
	BOOST_CHECK_EQUAL(files(makeRequest("/UPPER.TXT")).headers.at("Content-Type"), "text/plain");
	BOOST_CHECK_EQUAL(files(makeRequest("/.html")).headers.at("Content-Type"), "application/octet-stream");
	BOOST_CHECK_EQUAL(files(makeRequest("/missing.txt")).statusCode, 404);
	BOOST_CHECK_EQUAL(files(makeRequest("notes.txt")).statusCode, 400);
}

BOOST_AUTO_TEST_CASE(test_only_get_and_head_are_served) {
	root.write("page.txt", "hi");
	const StaticFiles files = openFiles({ .directory = root.path() });

	BOOST_CHECK_EQUAL(files(makeRequest("/page.txt", Method::HEAD)).statusCode, 200);
	const Response post = files(makeRequest("/page.txt", Method::POST));
	BOOST_CHECK_EQUAL(post.statusCode, 405);
	BOOST_CHECK_EQUAL(post.headers.at("Allow"), "GET, HEAD");
}

BOOST_AUTO_TEST_CASE(test_create_reports_an_unusable_directory) {
	root.write("file.txt", "not a directory");

	BOOST_CHECK(StaticFiles::create({ .directory = root.path() / "file.txt" }).error().code == std::errc::not_a_directory);
	BOOST_CHECK(StaticFiles::create({ .directory = root.path() / "missing" }).error().code == std::errc::no_such_file_or_directory);
}

BOOST_AUTO_TEST_CASE(test_directory_traversal_is_rejected) {
	root.write("secret.txt", "secret");
	std::filesystem::create_directory(root.path() / "www");
	root.write("www/public.txt", "public");

	const StaticFiles files = openFiles({ .directory = root.path() / "www" });

	BOOST_CHECK_EQUAL(files(makeRequest("/public.txt")).statusCode, 200);
	BOOST_CHECK_EQUAL(files(makeRequest("/../secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(files(makeRequest("/../../etc/passwd")).statusCode, 403);
	BOOST_CHECK_EQUAL(files(makeRequest("/subdir/../../etc/passwd")).statusCode, 403);
	BOOST_CHECK_EQUAL(files(parse("GET /..%2Fsecret.txt HTTP/1.1\r\n\r\n")).statusCode, 403);
}

BOOST_AUTO_TEST_CASE(test_skip_symlink_check_still_blocks_traversal) {
	root.write("secret.txt", "secret");
	std::filesystem::create_directory(root.path() / "www");
	root.write("www/public.txt", "public");
	std::filesystem::create_symlink(root.path() / "secret.txt", root.path() / "www/link.txt");

	const StaticFiles files = openFiles({ .directory = root.path() / "www", .skipSymlinkCheck = true });

	BOOST_CHECK_EQUAL(files(makeRequest("/public.txt")).body, "public");
	BOOST_CHECK_EQUAL(files(makeRequest("/link.txt")).body, "secret");
	BOOST_CHECK_EQUAL(files(makeRequest("/./nope/../public.txt")).body, "public");
	BOOST_CHECK_EQUAL(files(makeRequest("//public.txt")).body, "public");
	BOOST_CHECK_EQUAL(files(makeRequest("/../secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(files(makeRequest("/subdir/../../secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(files(parse("GET /..%2Fsecret.txt HTTP/1.1\r\n\r\n")).statusCode, 403);
}

BOOST_AUTO_TEST_CASE(test_directory_indexing_escapes_names) {
	root.write("a<script>.txt", "x");
	root.write("a b.txt", "x");
	std::filesystem::create_directories(root.path() / "sub/inner");
	root.write("sub/q&\"'.txt", "x");

	const StaticFiles filesOff = openFiles({ .directory = root.path() });
	BOOST_CHECK_EQUAL(filesOff(makeRequest("/")).statusCode, 403);

	const StaticFiles filesOn = openFiles({ .directory = root.path(), .enableDirectoryIndexing = true });
	const Response index = filesOn(makeRequest("/"));
	BOOST_CHECK_EQUAL(index.statusCode, 200);
	BOOST_CHECK(index.body.contains("&lt;script&gt;"));
	BOOST_CHECK(!index.body.contains("<script>"));
	BOOST_CHECK(index.body.contains("href=\"/a%20b.txt\""));
	BOOST_CHECK(index.body.contains(">a b.txt<"));
	BOOST_CHECK(!index.body.contains(">..<")); // The root has no parent link

	const std::string sub = filesOn(makeRequest("/sub")).body;
	BOOST_CHECK(sub.contains("<a href=\"/\">..</a>"));
	BOOST_CHECK(sub.contains(">q&amp;&quot;&#39;.txt<"));
	BOOST_CHECK(sub.contains("href=\"/sub/inner/\">inner/<"));

	BOOST_CHECK(filesOn(makeRequest("/sub/inner/")).body.contains("<a href=\"/sub/\">..</a>"));
}

BOOST_AUTO_TEST_CASE(test_preloaded_files_are_served_from_memory) {
	root.write("secret.txt", "secret");
	std::filesystem::create_directories(root.path() / "www/sub");
	root.write("www/small.txt", "small");
	root.write("www/sub/page.html", "<p>");
	root.write("www/big.txt", std::string(64, 'x'));
	std::filesystem::create_symlink(root.path() / "secret.txt", root.path() / "www/escape.txt");

	const StaticFiles files = openFiles({ .directory = root.path() / "www", .preloadBytes = 32 });

	root.write("www/small.txt", "changed");
	root.write("www/big.txt", "big");

	BOOST_CHECK_EQUAL(files(makeRequest("/small.txt")).body, "small");
	BOOST_CHECK_EQUAL(files(makeRequest("/big.txt")).body, "big");

	const Response nested = files(makeRequest("/sub/page.html"));
	BOOST_CHECK_EQUAL(nested.body, "<p>");
	BOOST_CHECK_EQUAL(nested.headers.at("Content-Type"), "text/html");

	BOOST_CHECK_EQUAL(files(makeRequest("/escape.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(files(makeRequest("/sub/../../secret.txt")).statusCode, 403);
}

BOOST_AUTO_TEST_SUITE_END()
