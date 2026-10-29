#include "Client.h"
#include "TestSupport.h"
#include "ou/http/Server.h"

#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/ssl.h>

using namespace ou::http;
using namespace ou::test;
namespace bdata = boost::unit_test::data;

namespace {

void sCloseTls(SSL *ssl) {
	SSL_shutdown(ssl);
	SSL_free(ssl);
}

// A TLS client that does not verify the peer, since the demo cert is self-signed
class TlsClient {
public:
	explicit TlsClient(uint16_t port) : client_(port) {
		SSL_set_fd(ssl_.get(), client_.fd());
		BOOST_REQUIRE_EQUAL(SSL_connect(ssl_.get()), 1);
	}

	void send(std::string_view request) const {
		BOOST_REQUIRE_GT(SSL_write(ssl_.get(), request.data(), static_cast<int>(request.size())), 0);
	}

	[[nodiscard]] std::string receive() const {
		return readResponse([this](std::span<char> buffer) { return SSL_read(ssl_.get(), buffer.data(), static_cast<int>(buffer.size())); });
	}

private:
	std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx_{ SSL_CTX_new(TLS_client_method()), &SSL_CTX_free };
	Client client_;
	std::unique_ptr<SSL, decltype(&sCloseTls)> ssl_{ SSL_new(ctx_.get()), &sCloseTls };
};

Server::Config sWithTls(Server::Config config) {
	config.tls = { .enabled = true,
		             .certPath = std::filesystem::path(TEST_CERT_DIR) / "cert.pem",
		             .keyPath = std::filesystem::path(TEST_CERT_DIR) / "key.pem" };
	return config;
}

bool sHandshakes(uint16_t port, int minVersion, int maxVersion, const char *ciphers = nullptr) {
	const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx{ SSL_CTX_new(TLS_client_method()), &SSL_CTX_free };
	BOOST_REQUIRE_EQUAL(SSL_CTX_set_min_proto_version(ctx.get(), minVersion), 1);
	BOOST_REQUIRE_EQUAL(SSL_CTX_set_max_proto_version(ctx.get(), maxVersion), 1);
	if (ciphers != nullptr)
		BOOST_REQUIRE_EQUAL(SSL_CTX_set_cipher_list(ctx.get(), ciphers), 1);
	const Client client(port);
	const std::unique_ptr<SSL, decltype(&SSL_free)> ssl{ SSL_new(ctx.get()), &SSL_free };
	SSL_set_fd(ssl.get(), client.fd());
	return SSL_connect(ssl.get()) == 1;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(tls, ServingRoot)

BOOST_DATA_TEST_CASE(test_tls, bdata::make(kModels), model) {
	root.write("a.txt", "over tls");

	const Server server = startServer(sWithTls(makeConfig(model)), files());
	const uint16_t port = server.port();

	{ // Keep-alive over TLS: two requests on one session
		const TlsClient client(port);
		client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
		const std::string response = client.receive();
		BOOST_CHECK(response.starts_with("HTTP/1.1 200 OK"));
		BOOST_CHECK(response.ends_with("over tls"));

		client.send("GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
		BOOST_CHECK(client.receive().starts_with("HTTP/1.1 404"));
	}

	if (model != Server::Model::EventLoop)
		return;

	std::vector<TlsClient> clients;
	clients.reserve(8);
	for (int i = 0; i < 8; ++i)
		clients.emplace_back(port);

	for (const TlsClient &client : clients)
		client.send("GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
	for (const TlsClient &client : clients)
		BOOST_CHECK(client.receive().ends_with("over tls"));
}

// One TLS record larger than a read chunk: the rest waits in OpenSSL, unseen by the poller
BOOST_DATA_TEST_CASE(test_tls_request_larger_than_a_read_chunk, bdata::make(kModels), model) {
	const auto echo = [](const Request &request) {
		return Response::plainText(200, std::string(request.body));
	};
	const Server server = startServer(sWithTls(makeConfig(model)), Router().route("/echo", put(echo)));

	const std::string body(size_t{ 10 } * 1024, 'x');
	const TlsClient client(server.port());
	client.send(std::format("PUT /echo HTTP/1.1\r\nHost: x\r\nContent-Length: {}\r\n\r\n{}", body.size(), body));
	BOOST_CHECK(client.receive().ends_with(body));
}

BOOST_AUTO_TEST_CASE(test_protocol_and_cipher_policy) {
	const Server server = startServer(sWithTls(makeConfig(Server::Model::EventLoop)));
	const uint16_t port = server.port();

	BOOST_CHECK(sHandshakes(port, TLS1_2_VERSION, TLS1_2_VERSION));
	BOOST_CHECK(sHandshakes(port, TLS1_3_VERSION, TLS1_3_VERSION));

	// @SECLEVEL=0 lets the client offer weak parameters, so a failure is the server's policy
	BOOST_CHECK(!sHandshakes(port, TLS1_VERSION, TLS1_1_VERSION, "ALL:@SECLEVEL=0"));
	BOOST_CHECK(!sHandshakes(port, TLS1_2_VERSION, TLS1_2_VERSION, "AES256-SHA:ECDHE-RSA-AES256-SHA:@SECLEVEL=0"));
}

BOOST_AUTO_TEST_CASE(test_start_reports_bad_certificate) {
	Server::Config missing = sWithTls(makeConfig(Server::Model::EventLoop));
	missing.tls.certPath = root.path() / "missing.pem";
	const auto started = Server::start(missing);
	BOOST_REQUIRE(!started);
	BOOST_CHECK(started.error().step.starts_with("initialize TLS"));
}

// Checks the socket timeouts are set before the handshake; needs TLS, since a plain-socket handshake is a no-op
BOOST_AUTO_TEST_CASE(test_blocking_loop_times_out_silent_tls_peer) {
	Server::Config silent = sWithTls(makeConfig(Server::Model::ThreadPerConnection));
	silent.idleTimeout = std::chrono::seconds(1);
	const Server server = startServer(silent);

	Client(server.port()).checkClosed();
}

BOOST_AUTO_TEST_SUITE_END()
