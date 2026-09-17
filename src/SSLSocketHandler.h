#pragma once

#ifndef DISABLE_HTTPS

#include "SocketHandler.h"

#include <filesystem>
#include <span>
#include <string_view>

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

	IoResult acceptConnection(int clientSocket) override final;
	IoResult read(int clientSocket, std::span<char> buffer) override final;
	IoResult write(int clientSocket, std::string_view data) override final;
	void closeConnection(int clientSocket) override final;

private:
	SSL_CTX *sslCtx_;
};

} // namespace ou

#endif // DISABLE_HTTPS
