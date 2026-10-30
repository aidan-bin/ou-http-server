#pragma once

#include "SocketIo.h"

#include <filesystem>
#include <span>
#include <string_view>

#include <openssl/ssl.h>

namespace ou::http {

class TlsIo final : public SocketIo {
public:
	TlsIo(const std::filesystem::path &certPath, const std::filesystem::path &keyPath);
	TlsIo(const TlsIo &) = delete;
	TlsIo &operator=(const TlsIo &) = delete;
	TlsIo(TlsIo &&) = delete;
	TlsIo &operator=(TlsIo &&) = delete;
	~TlsIo() override;

	IoStatus handshake(int clientSocket) final;
	IoResult read(int clientSocket, std::span<char> buffer) final;
	bool hasBuffered(int clientSocket) final;
	IoResult write(int clientSocket, std::string_view data) final;
	void discardSession(int clientSocket) final;

private:
	SSL_CTX *sslCtx_;
};

} // namespace ou::http
