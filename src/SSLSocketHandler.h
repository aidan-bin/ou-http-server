#pragma once

#include "SocketHandler.h"

#include <filesystem>
#include <span>
#include <string_view>

#include <openssl/ssl.h>

namespace ou {

class SSLSocketHandler final : public SocketHandler {
public:
	SSLSocketHandler(const std::filesystem::path &certPath, const std::filesystem::path &keyPath);
	SSLSocketHandler(const SSLSocketHandler &) = delete;
	SSLSocketHandler &operator=(const SSLSocketHandler &) = delete;
	SSLSocketHandler(SSLSocketHandler &&) = delete;
	SSLSocketHandler &operator=(SSLSocketHandler &&) = delete;
	~SSLSocketHandler() override;

	IoResult handshake(int clientSocket) final;
	IoResult read(int clientSocket, std::span<char> buffer) final;
	IoResult write(int clientSocket, std::string_view data) final;
	void discardSession(int clientSocket) final;

private:
	SSL_CTX *sslCtx_;
};

} // namespace ou
