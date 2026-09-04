#include "SSLSocketHandler.h"

#ifndef DISABLE_HTTPS

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace ou {

namespace {

	// Large transfers are chunked since SSL_write() only takes an int
	constexpr size_t kMaxTransfer = static_cast<size_t>(std::numeric_limits<int>::max());

} // namespace

SSLSocketHandler::SSLSocketHandler(const Config &config) : sslCtx_(SSL_CTX_new(TLS_server_method())) {
	if ((sslCtx_ == nullptr) || SSL_CTX_use_certificate_file(sslCtx_, config.certPath.c_str(), SSL_FILETYPE_PEM) <= 0
			|| SSL_CTX_use_PrivateKey_file(sslCtx_, config.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
		SSL_CTX_free(sslCtx_);
		throw std::runtime_error("Failed to initialize SSL.");
	}
}

SSLSocketHandler::~SSLSocketHandler() {
	for (const auto &[clientSocket, ssl] : sslSessions_) {
		SSL_free(ssl);
	}
	SSL_CTX_free(sslCtx_);
}

SSL *SSLSocketHandler::session(int clientSocket) const {
	const std::lock_guard<std::mutex> lock(sessionsMutex_);
	const auto it = sslSessions_.find(clientSocket);
	return it != sslSessions_.end() ? it->second : nullptr;
}

bool SSLSocketHandler::acceptConnection(int clientSocket) {
	SSL *ssl = SSL_new(sslCtx_);
	if (ssl == nullptr)
		return false;

	SSL_set_fd(ssl, clientSocket);
	if (SSL_accept(ssl) <= 0) {
		SSL_free(ssl);
		return false;
	}

	const std::lock_guard<std::mutex> lock(sessionsMutex_);
	sslSessions_[clientSocket] = ssl;
	return true;
}

ssize_t SSLSocketHandler::read(int clientSocket, std::span<char> buffer) {
	// The lock covers the map lookup only: one thread owns a connection, so TLS I/O runs outside it
	SSL *ssl = session(clientSocket);
	if (ssl == nullptr)
		return -1;

	const auto size = static_cast<int>(std::min(buffer.size(), kMaxTransfer));
	return SSL_read(ssl, buffer.data(), size);
}

ssize_t SSLSocketHandler::write(int clientSocket, std::string_view data) {
	SSL *ssl = session(clientSocket);
	if (ssl == nullptr)
		return -1;

	size_t written = 0;
	while (written < data.size()) {
		const std::string_view remaining = data.substr(written);
		const auto size = static_cast<int>(std::min(remaining.size(), kMaxTransfer));

		const int sent = SSL_write(ssl, remaining.data(), size);
		if (sent <= 0)
			return -1;

		written += static_cast<size_t>(sent);
	}

	return static_cast<ssize_t>(written);
}

void SSLSocketHandler::closeConnection(int clientSocket) {
	SSL *ssl = nullptr;
	{
		const std::lock_guard<std::mutex> lock(sessionsMutex_);
		const auto it = sslSessions_.find(clientSocket);
		if (it != sslSessions_.end()) {
			ssl = it->second;
			sslSessions_.erase(it);
		}
	}

	if (ssl != nullptr) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
	}

	::close(clientSocket);
}

} // namespace ou

#endif // DISABLE_HTTPS
