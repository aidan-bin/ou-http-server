#pragma once

#ifndef DISABLE_HTTPS

#include "SocketHandler.h"

#include <filesystem>
#include <string>
#include <unordered_map>

#include <openssl/err.h>
#include <openssl/ssl.h>

class SSLSocketHandler : public SocketHandler {
public:
	struct Config {
		bool enabled = false;
		std::filesystem::path certPath;
		std::filesystem::path keyPath;
	};

	explicit SSLSocketHandler(const Config &config);
	~SSLSocketHandler() override final;

	bool acceptConnection(int clientSocket) override final;
	ssize_t read(int clientSocket, char *buffer, size_t size) override final;
	ssize_t write(int clientSocket, const std::string &data) override final;
	void closeConnection(int clientSocket) override final;

private:
	SSL_CTX *sslCtx_;
	std::unordered_map<int, SSL *> sslSessions_;
};

#endif // DISABLE_HTTPS
