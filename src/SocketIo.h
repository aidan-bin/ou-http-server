#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ou::http {

enum class IoStatus : uint8_t { Ok, WantRead, WantWrite, Closed, Error };

struct IoResult {
	IoStatus status = IoStatus::Ok;
	size_t bytes = 0;
};

class SocketIo {
public:
	SocketIo() = default;
	SocketIo(const SocketIo &) = delete;
	SocketIo &operator=(const SocketIo &) = delete;
	SocketIo(SocketIo &&) = delete;
	SocketIo &operator=(SocketIo &&) = delete;
	virtual ~SocketIo() = default;

	// Call once per new connection before reading; a no-op for plain sockets.
	// A TLS handshake still in flight reports WantRead/WantWrite and resumes on the next call.
	virtual IoResult handshake(int clientSocket) = 0;

	virtual IoResult read(int clientSocket, std::span<char> buffer) = 0;

	// Whether read() holds bytes the poller cannot see (TLS)
	[[nodiscard]] virtual bool hasBuffered(int clientSocket) {
		(void)clientSocket;
		return false;
	}

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

} // namespace ou::http
