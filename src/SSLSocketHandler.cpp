#include "SSLSocketHandler.h"

#ifndef DISABLE_HTTPS

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <stdexcept>
#include <unistd.h>

#include <openssl/err.h>

namespace ou {

namespace {

	// Large transfers are chunked since SSL_write() only takes an int
	constexpr size_t kMaxTransfer = static_cast<size_t>(std::numeric_limits<int>::max());

	// Per-thread because a worker serves one connection at a time and belongs to a single SSLSocketHandler
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) thread-local by design
	thread_local SSL *g_session = nullptr;

} // namespace

SSLSocketHandler::SSLSocketHandler(const Config &config) : sslCtx_(SSL_CTX_new(TLS_server_method())) {
	if ((sslCtx_ == nullptr) || SSL_CTX_use_certificate_file(sslCtx_, config.certPath.c_str(), SSL_FILETYPE_PEM) <= 0
			|| SSL_CTX_use_PrivateKey_file(sslCtx_, config.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
		SSL_CTX_free(sslCtx_);
		std::array<char, 256> reason{};
		ERR_error_string_n(ERR_get_error(), reason.data(), reason.size());
		throw std::runtime_error(std::format("Failed to initialize SSL ({})", reason.data()));
	}
}

SSLSocketHandler::~SSLSocketHandler() { SSL_CTX_free(sslCtx_); }

bool SSLSocketHandler::acceptConnection(int clientSocket) {
	SSL *ssl = SSL_new(sslCtx_);
	if (ssl == nullptr)
		return false;

	SSL_set_fd(ssl, clientSocket);
	if (SSL_accept(ssl) <= 0) {
		SSL_free(ssl);
		return false;
	}

	g_session = ssl;
	return true;
}

ssize_t SSLSocketHandler::read(int clientSocket, std::span<char> buffer) {
	(void)clientSocket;
	if (g_session == nullptr)
		return -1;

	const auto size = static_cast<int>(std::min(buffer.size(), kMaxTransfer));
	return SSL_read(g_session, buffer.data(), size);
}

ssize_t SSLSocketHandler::write(int clientSocket, std::string_view data) {
	(void)clientSocket;
	if (g_session == nullptr)
		return -1;

	size_t written = 0;
	while (written < data.size()) {
		const std::string_view remaining = data.substr(written);
		const auto size = static_cast<int>(std::min(remaining.size(), kMaxTransfer));

		const int sent = SSL_write(g_session, remaining.data(), size);
		if (sent <= 0)
			return -1;

		written += static_cast<size_t>(sent);
	}

	return static_cast<ssize_t>(written);
}

void SSLSocketHandler::closeConnection(int clientSocket) {
	if (g_session != nullptr) {
		SSL_shutdown(g_session);
		SSL_free(g_session);
		g_session = nullptr;
	}

	::close(clientSocket);
}

} // namespace ou

#endif // DISABLE_HTTPS
