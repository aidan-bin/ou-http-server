#include "Client.h"
#include "TestSupport.h"
#include "ou/http/Server.h"

#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <openssl/ssl.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
	config.https = { .enabled = true,
		               .certPath = std::filesystem::path(TEST_CERT_DIR) / "cert.pem",
		               .keyPath = std::filesystem::path(TEST_CERT_DIR) / "key.pem" };
	return config;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(tls, ServingRoot)

BOOST_DATA_TEST_CASE(test_tls, bdata::make(kModels), model) {
	root.write("a.txt", "over tls");

	const Server server = startServer(sWithTls(config(model)), files());
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

BOOST_AUTO_TEST_CASE(test_start_reports_bad_certificate) {
	Server::Config missing = sWithTls(config(Server::Model::EventLoop));
	missing.https.certPath = root.path() / "missing.pem";
	const auto started = Server::start(missing);
	BOOST_REQUIRE(!started);
	BOOST_CHECK(started.error().step.starts_with("initialize socket handler"));
}

// Checks the socket timeouts are set before the handshake; needs TLS, since a plain-socket handshake is a no-op
BOOST_AUTO_TEST_CASE(test_blocking_loop_times_out_silent_tls_peer) {
	Server::Config silent = sWithTls(config(Server::Model::ThreadPerCore));
	silent.readTimeout = std::chrono::seconds(1);
	const Server server = startServer(silent);

	Client(server.port()).checkClosed();
}

BOOST_AUTO_TEST_SUITE_END()
