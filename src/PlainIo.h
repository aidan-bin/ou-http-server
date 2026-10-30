#pragma once

#include "SocketIo.h"

#include <cerrno>
#include <cstddef>
#include <span>
#include <string_view>

#include <sys/socket.h>
#include <unistd.h>

namespace ou::http {

class PlainIo final : public SocketIo {
public:
	IoResult handshake(int clientSocket) final {
		(void)clientSocket;
		return {};
	}

	IoResult read(int clientSocket, std::span<char> buffer) final {
		ssize_t bytesRead = 0;
		while ((bytesRead = ::read(clientSocket, buffer.data(), buffer.size())) < 0 && errno == EINTR) {
		}

		if (bytesRead > 0)
			return { .status = IoStatus::Ok, .bytes = static_cast<size_t>(bytesRead) };
		if (bytesRead == 0)
			return { .status = IoStatus::Closed };

		return { .status = wouldBlock() ? IoStatus::WantRead : IoStatus::Error };
	}

	IoResult write(int clientSocket, std::string_view data) final {
		ssize_t sent = 0;
		while ((sent = ::send(clientSocket, data.data(), data.size(), 0)) < 0 && errno == EINTR) {
		}

		if (sent > 0)
			return { .status = IoStatus::Ok, .bytes = static_cast<size_t>(sent) };
		if (sent == 0)
			return { .status = IoStatus::Closed };

		return { .status = wouldBlock() ? IoStatus::WantWrite : IoStatus::Error };
	}

	void discardSession(int clientSocket) final { (void)clientSocket; }

private:
	static bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
};

} // namespace ou::http
