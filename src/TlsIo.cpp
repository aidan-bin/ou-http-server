#include "TlsIo.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include <openssl/err.h>

namespace ou::http {

namespace {

	// Large transfers are chunked since SSL_write() only takes an int
	constexpr size_t kMaxTransfer = static_cast<size_t>(std::numeric_limits<int>::max());

	// Each connection's TLS session, by fd.
	// Thread-local needs no lock since a connection never leaves its thread.
	thread_local std::unordered_map<int, SSL *> g_sessions;

	SSL *sSession(int clientSocket) {
		const auto session = g_sessions.find(clientSocket);
		return session != g_sessions.end() ? session->second : nullptr;
	}

	void sDiscardSession(int clientSocket) {
		const auto session = g_sessions.find(clientSocket);
		if (session == g_sessions.end())
			return;

		SSL_free(session->second);
		g_sessions.erase(session);
	}

	// Leaves the thread's error queue empty, or the next SSL_get_error() on this thread misreads it
	IoResult sStatusFor(SSL *ssl, int result) {
		const int error = SSL_get_error(ssl, result);
		ERR_clear_error();

		switch (error) {
		case SSL_ERROR_WANT_READ:
			return { .status = IoStatus::WantRead };
		case SSL_ERROR_WANT_WRITE:
			return { .status = IoStatus::WantWrite };
		case SSL_ERROR_ZERO_RETURN:
			return { .status = IoStatus::Closed };
		default:
			return { .status = IoStatus::Error };
		}
	}

} // namespace

TlsIo::TlsIo(const std::filesystem::path &certPath, const std::filesystem::path &keyPath) : sslCtx_(SSL_CTX_new(TLS_server_method())) {
	// Minimum TLS 1.2, restricted to ECDHE + AES-GCM ciphers (TLS 1.3 suites are all AEAD, left at default)
	if ((sslCtx_ == nullptr) || SSL_CTX_set_min_proto_version(sslCtx_, TLS1_2_VERSION) != 1
	    || SSL_CTX_set_cipher_list(sslCtx_, "ECDHE+AESGCM") != 1 || SSL_CTX_set1_groups_list(sslCtx_, "X25519:prime256v1:secp384r1") != 1
	    || SSL_CTX_use_certificate_file(sslCtx_, certPath.c_str(), SSL_FILETYPE_PEM) <= 0
	    || SSL_CTX_use_PrivateKey_file(sslCtx_, keyPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
		SSL_CTX_free(sslCtx_);
		std::array<char, 256> reason{};
		ERR_error_string_n(ERR_get_error(), reason.data(), reason.size());
		throw std::runtime_error(std::format("Failed to initialize SSL ({})", reason.data()));
	}
	SSL_CTX_set_options(sslCtx_, SSL_OP_NO_RENEGOTIATION);
	SSL_CTX_set_mode(sslCtx_, SSL_MODE_RELEASE_BUFFERS);
}

TlsIo::~TlsIo() { SSL_CTX_free(sslCtx_); }

IoResult TlsIo::handshake(int clientSocket) {
	SSL *ssl = sSession(clientSocket);
	if (ssl == nullptr) {
		ssl = SSL_new(sslCtx_);
		if (ssl == nullptr)
			return { .status = IoStatus::Error };

		SSL_set_fd(ssl, clientSocket);
		g_sessions.emplace(clientSocket, ssl);
	}

	const int result = SSL_accept(ssl);
	if (result == 1)
		return {};

	const IoResult status = sStatusFor(ssl, result);
	if (status.status != IoStatus::WantRead && status.status != IoStatus::WantWrite)
		sDiscardSession(clientSocket);

	return status;
}

IoResult TlsIo::read(int clientSocket, std::span<char> buffer) {
	SSL *ssl = sSession(clientSocket);
	if (ssl == nullptr)
		return { .status = IoStatus::Error };

	const auto size = static_cast<int>(std::min(buffer.size(), kMaxTransfer));
	const int bytesRead = SSL_read(ssl, buffer.data(), size);
	if (bytesRead > 0)
		return { .status = IoStatus::Ok, .bytes = static_cast<size_t>(bytesRead) };

	return sStatusFor(ssl, bytesRead);
}

bool TlsIo::hasBuffered(int clientSocket) {
	SSL *ssl = sSession(clientSocket);
	return ssl != nullptr && SSL_has_pending(ssl) == 1;
}

IoResult TlsIo::write(int clientSocket, std::string_view data) {
	SSL *ssl = sSession(clientSocket);
	if (ssl == nullptr)
		return { .status = IoStatus::Error };

	// Partial writes are off: all of this chunk goes out, or none of it
	const auto size = static_cast<int>(std::min(data.size(), kMaxTransfer));
	const int sent = SSL_write(ssl, data.data(), size);
	if (sent > 0)
		return { .status = IoStatus::Ok, .bytes = static_cast<size_t>(sent) };

	return sStatusFor(ssl, sent);
}

void TlsIo::discardSession(int clientSocket) {
	SSL *ssl = sSession(clientSocket);
	if (ssl == nullptr)
		return;

	SSL_shutdown(ssl);
	ERR_clear_error();
	sDiscardSession(clientSocket);
}

} // namespace ou::http
