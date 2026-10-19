#include "HttpTypes.h"
#include "TestSupport.h"

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

	const TestServer server(config());

	const Response html = server.handleStaticFileRequest(makeRequest("/page.html"));
	BOOST_CHECK_EQUAL(html.statusCode, 200);
	BOOST_CHECK_EQUAL(html.body, "<h1>hi</h1>");
	BOOST_CHECK_EQUAL(html.headers.at("Content-Type"), "text/html");

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/notes.txt")).headers.at("Content-Type"), "text/plain");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/blob.bin")).headers.at("Content-Type"), "application/octet-stream");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/UPPER.TXT")).headers.at("Content-Type"), "text/plain");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/.html")).headers.at("Content-Type"), "application/octet-stream");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/missing.txt")).statusCode, 404);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("notes.txt")).statusCode, 400);
}

BOOST_AUTO_TEST_CASE(test_directory_traversal_is_rejected) {
	root.write("secret.txt", "secret");
	std::filesystem::create_directory(root.path() / "www");
	root.write("www/public.txt", "public");

	const TestServer server(makeConfig(root.path() / "www"));

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/public.txt")).statusCode, 200);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/../secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/../../etc/passwd")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/subdir/../../etc/passwd")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(parse("GET /..%2Fsecret.txt HTTP/1.1\r\n\r\n")).statusCode, 403);
}

BOOST_AUTO_TEST_CASE(test_skip_symlink_check_still_blocks_traversal) {
	root.write("secret.txt", "secret");
	std::filesystem::create_directory(root.path() / "www");
	root.write("www/public.txt", "public");
	std::filesystem::create_symlink(root.path() / "secret.txt", root.path() / "www/link.txt");

	Server::Config skipping = makeConfig(root.path() / "www");
	skipping.skipSymlinkCheck = true;
	const TestServer server(skipping);

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/public.txt")).body, "public");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/link.txt")).body, "secret");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/./nope/../public.txt")).body, "public");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("//public.txt")).body, "public");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/../secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/subdir/../../secret.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(parse("GET /..%2Fsecret.txt HTTP/1.1\r\n\r\n")).statusCode, 403);
}

BOOST_AUTO_TEST_CASE(test_directory_indexing_escapes_names) {
	root.write("a<script>.txt", "x");
	root.write("a b.txt", "x");
	std::filesystem::create_directories(root.path() / "sub/inner");
	root.write("sub/q&\"'.txt", "x");

	const TestServer serverOff(config());
	BOOST_CHECK_EQUAL(serverOff.handleStaticFileRequest(makeRequest("/")).statusCode, 403);

	Server::Config indexing = config();
	indexing.enableDirectoryIndexing = true;
	const TestServer serverOn(indexing);
	const Response index = serverOn.handleStaticFileRequest(makeRequest("/"));
	BOOST_CHECK_EQUAL(index.statusCode, 200);
	BOOST_CHECK(index.body.contains("&lt;script&gt;"));
	BOOST_CHECK(!index.body.contains("<script>"));
	BOOST_CHECK(index.body.contains("href=\"/a%20b.txt\""));
	BOOST_CHECK(index.body.contains(">a b.txt<"));
	BOOST_CHECK(!index.body.contains(">..<")); // The root has no parent link

	const std::string sub = serverOn.handleStaticFileRequest(makeRequest("/sub")).body;
	BOOST_CHECK(sub.contains("<a href=\"/\">..</a>"));
	BOOST_CHECK(sub.contains(">q&amp;&quot;&#39;.txt<"));
	BOOST_CHECK(sub.contains("href=\"/sub/inner/\">inner/<"));

	BOOST_CHECK(serverOn.handleStaticFileRequest(makeRequest("/sub/inner/")).body.contains("<a href=\"/sub/\">..</a>"));
}

BOOST_AUTO_TEST_CASE(test_preloaded_files_are_served_from_memory) {
	root.write("secret.txt", "secret");
	std::filesystem::create_directories(root.path() / "www/sub");
	root.write("www/small.txt", "small");
	root.write("www/sub/page.html", "<p>");
	root.write("www/big.txt", std::string(64, 'x'));
	std::filesystem::create_symlink(root.path() / "secret.txt", root.path() / "www/escape.txt");

	Server::Config preloading = makeConfig(root.path() / "www");
	preloading.preloadBytes = 32;
	const TestServer server(preloading);

	root.write("www/small.txt", "changed");
	root.write("www/big.txt", "big");

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/small.txt")).body, "small");
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/big.txt")).body, "big");

	const Response nested = server.handleStaticFileRequest(makeRequest("/sub/page.html"));
	BOOST_CHECK_EQUAL(nested.body, "<p>");
	BOOST_CHECK_EQUAL(nested.headers.at("Content-Type"), "text/html");

	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/escape.txt")).statusCode, 403);
	BOOST_CHECK_EQUAL(server.handleStaticFileRequest(makeRequest("/sub/../../secret.txt")).statusCode, 403);
}

BOOST_AUTO_TEST_SUITE_END()
