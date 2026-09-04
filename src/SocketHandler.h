#pragma once

#include <cerrno>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace ou {

class SocketHandler {
public:
	SocketHandler() = default;
	SocketHandler(const SocketHandler &) = delete;
	SocketHandler &operator=(const SocketHandler &) = delete;
	SocketHandler(SocketHandler &&) = delete;
	SocketHandler &operator=(SocketHandler &&) = delete;
	virtual ~SocketHandler() = default;

	virtual bool acceptConnection(int clientSocket) = 0;
	virtual ssize_t read(int clientSocket, std::span<char> buffer) = 0;
	virtual ssize_t write(int clientSocket, std::string_view data) = 0;
	virtual void closeConnection(int clientSocket) = 0;
};

class PlainSocketHandler : public SocketHandler {
public:
	bool acceptConnection(int clientSocket) override final {
		(void)clientSocket;
		return true;
	}

	ssize_t read(int clientSocket, std::span<char> buffer) override final { return ::read(clientSocket, buffer.data(), buffer.size()); }

	ssize_t write(int clientSocket, std::string_view data) override final {
		size_t written = 0;
		while (written < data.size()) {
			const std::string_view remaining = data.substr(written);
			const ssize_t sent = ::send(clientSocket, remaining.data(), remaining.size(), 0);
			if (sent < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (sent == 0)
				break;
			written += static_cast<size_t>(sent);
		}
		return static_cast<ssize_t>(written);
	}

	void closeConnection(int clientSocket) override final { ::close(clientSocket); }
};

} // namespace ou
