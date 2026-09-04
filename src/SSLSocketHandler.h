#pragma once

#ifndef DISABLE_HTTPS

#include "SocketHandler.h"

#include <filesystem>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace ou {

class SSLSocketHandler final : public SocketHandler {
public:
	struct Config {
		bool enabled = false;
		std::filesystem::path certPath;
		std::filesystem::path keyPath;
	};

	explicit SSLSocketHandler(const Config &config);
	SSLSocketHandler(const SSLSocketHandler &) = delete;
	SSLSocketHandler &operator=(const SSLSocketHandler &) = delete;
	SSLSocketHandler(SSLSocketHandler &&) = delete;
	SSLSocketHandler &operator=(SSLSocketHandler &&) = delete;
	~SSLSocketHandler() override;

	bool acceptConnection(int clientSocket) override final;
	ssize_t read(int clientSocket, std::span<char> buffer) override final;
	ssize_t write(int clientSocket, std::string_view data) override final;
	void closeConnection(int clientSocket) override final;

private:
	// nullptr if there is no session for this socket
	[[nodiscard]] SSL *session(int clientSocket) const;

	SSL_CTX *sslCtx_;

	// Shared by every worker thread, so the map needs the mutex
	mutable std::mutex sessionsMutex_;
	std::unordered_map<int, SSL *> sslSessions_;
};

} // namespace ou

#endif // DISABLE_HTTPS
