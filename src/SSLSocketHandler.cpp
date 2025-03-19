#include "SSLSocketHandler.h"

#include <stdexcept>
#include <unistd.h>

SSLSocketHandler::SSLSocketHandler(const Config& config) {
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();
	sslCtx_ = SSL_CTX_new(TLS_server_method());

	if (!sslCtx_ || 
	    SSL_CTX_use_certificate_file(sslCtx_, config.certPath.c_str(), SSL_FILETYPE_PEM) <= 0 ||
	    SSL_CTX_use_PrivateKey_file(sslCtx_, config.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
		throw std::runtime_error("Failed to initialize SSL.");
	}
}

SSLSocketHandler::~SSLSocketHandler() {
	SSL_CTX_free(sslCtx_);
}

bool SSLSocketHandler::acceptConnection(int clientSocket) {
	SSL* ssl = SSL_new(sslCtx_);
	SSL_set_fd(ssl, clientSocket);
	if (SSL_accept(ssl) <= 0) {
		SSL_free(ssl);
		return false;
	}
	sslSessions_[clientSocket] = ssl;
	return true;
}

ssize_t SSLSocketHandler::read(int clientSocket, char* buffer, size_t size) {
	auto it = sslSessions_.find(clientSocket);
	if (it == sslSessions_.end()) return -1;
	return SSL_read(it->second, buffer, size);
}

ssize_t SSLSocketHandler::write(int clientSocket, const std::string& data) {
	auto it = sslSessions_.find(clientSocket);
	if (it == sslSessions_.end()) return -1;
	return SSL_write(it->second, data.c_str(), data.size());
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
