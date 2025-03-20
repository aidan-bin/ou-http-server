#include "SSLSocketHandler.h"

#include <limits>
#include <stdexcept>
#include <unistd.h>

SSLSocketHandler::SSLSocketHandler(const Config &config) : sslCtx_(SSL_CTX_new(TLS_server_method())) {
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	if ((sslCtx_ == nullptr) || SSL_CTX_use_certificate_file(sslCtx_, config.certPath.c_str(), SSL_FILETYPE_PEM) <= 0
			|| SSL_CTX_use_PrivateKey_file(sslCtx_, config.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
		throw std::runtime_error("Failed to initialize SSL.");
	}
}

SSLSocketHandler::~SSLSocketHandler() { SSL_CTX_free(sslCtx_); }

bool SSLSocketHandler::acceptConnection(int clientSocket) {
	SSL *ssl = SSL_new(sslCtx_);
	SSL_set_fd(ssl, clientSocket);
	if (SSL_accept(ssl) <= 0) {
		SSL_free(ssl);
		return false;
	}
	sslSessions_[clientSocket] = ssl;
	return true;
}

ssize_t SSLSocketHandler::read(int clientSocket, char *buffer, size_t size) {
	auto it = sslSessions_.find(clientSocket);
	if (it == sslSessions_.end())
		return -1;
	if (size > std::numeric_limits<int>::max())
		throw std::overflow_error("Buffer size exceeds maximum int value for SSL_read");
	return SSL_read(it->second, buffer, static_cast<int>(size));
}

ssize_t SSLSocketHandler::write(int clientSocket, const std::string &data) {
	auto it = sslSessions_.find(clientSocket);
	if (it == sslSessions_.end())
		return -1;
	if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		throw std::overflow_error("Data size exceeds maximum int value for SSL_write");
	return SSL_write(it->second, data.c_str(), static_cast<int>(data.size()));
}

void SSLSocketHandler::closeConnection(int clientSocket) {
	auto it = sslSessions_.find(clientSocket);
	if (it != sslSessions_.end()) {
		SSL_shutdown(it->second);
		SSL_free(it->second);
		sslSessions_.erase(it);
	}
	::close(clientSocket);
}
