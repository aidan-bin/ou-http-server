#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ou::http {

enum class IoStatus : uint8_t { Ok, WantRead, WantWrite, Closed, Error };

struct IoResult {
	IoStatus status = IoStatus::Ok;
	size_t bytes = 0;
};

inline bool setNonBlocking(int fd, bool enable) {
	const int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return false;

	const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return ::fcntl(fd, F_SETFL, updated) >= 0;
}

class SocketHandler {
public:
	SocketHandler() = default;
	SocketHandler(const SocketHandler &) = delete;
	SocketHandler &operator=(const SocketHandler &) = delete;
	SocketHandler(SocketHandler &&) = delete;
	SocketHandler &operator=(SocketHandler &&) = delete;
	virtual ~SocketHandler() = default;

	// Call once per new connection before reading; a no-op for plain sockets.
	// A TLS handshake still in flight reports WantRead/WantWrite and resumes on the next call.
	virtual IoResult handshake(int clientSocket) = 0;

	virtual IoResult read(int clientSocket, std::span<char> buffer) = 0;

	// May write less than asked; the caller resumes from result.bytes
	virtual IoResult write(int clientSocket, std::string_view data) = 0;

	// Releases whatever the handler holds for this socket; caller owns the fd and closes it
	virtual void discardSession(int clientSocket) = 0;

	// Unlike write(), retries until everything is sent.
	// Blocking sockets only: on a non-blocking one it gives up at the first WantWrite.
	IoResult writeAll(int clientSocket, std::string_view data) {
		size_t written = 0;
		while (written < data.size()) {
			const IoResult result = write(clientSocket, data.substr(written));
			written += result.bytes;

			if (result.status != IoStatus::Ok)
				return { .status = result.status, .bytes = written };
		}

		return { .status = IoStatus::Ok, .bytes = written };
	}
};

class PlainSocketHandler final : public SocketHandler {
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
