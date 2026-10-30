#include "Listener.h"

#include <array>
#include <cerrno>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ou::http {

namespace {

	// Reads errno, so call it before anything (like closing the socket) can overwrite it
	Error sErrnoError(std::string_view step) { return { .step = std::string(step), .code = std::error_code(errno, std::system_category()) }; }

	bool sIsTransientAcceptError(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == ECONNABORTED || error == EINTR; }

} // namespace

std::string addressToString(const sockaddr_in &address) {
	std::array<char, INET_ADDRSTRLEN> buffer{};
	if (inet_ntop(AF_INET, &address.sin_addr, buffer.data(), buffer.size()) == nullptr)
		return "unknown";
	return { buffer.data() };
}

bool setNonBlocking(int fd, bool enable) {
	const int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return false;

	const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return ::fcntl(fd, F_SETFL, updated) >= 0;
}

std::expected<Listener, Error> openListener(uint16_t port) {
	UniqueFd socket(::socket(AF_INET, SOCK_STREAM, 0));
	if (!socket)
		return std::unexpected(sErrnoError("create socket"));

	// Lets a restarted server rebind while the old socket's connections sit in TIME_WAIT
	const int enable = 1;
	if (setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
		return std::unexpected(sErrnoError("set SO_REUSEADDR"));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);
	if (bind(socket.get(), reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
		return std::unexpected(sErrnoError("bind socket"));

	if (listen(socket.get(), SOMAXCONN) < 0)
		return std::unexpected(sErrnoError("listen on socket"));

	if (!setNonBlocking(socket.get(), true))
		return std::unexpected(sErrnoError("set socket non-blocking"));

	socklen_t length = sizeof(addr);
	if (getsockname(socket.get(), reinterpret_cast<struct sockaddr *>(&addr), &length) < 0)
		return std::unexpected(sErrnoError("read the bound port"));

	return Listener{ .socket = std::move(socket), .port = ntohs(addr.sin_port) };
}

std::expected<UniqueFd, std::error_code> acceptClient(int listenSocket, sockaddr_in &clientAddr) {
	socklen_t length = sizeof(clientAddr);
	UniqueFd client(::accept(listenSocket, reinterpret_cast<struct sockaddr *>(&clientAddr), &length));
	if (client || sIsTransientAcceptError(errno))
		return client;
	return std::unexpected(std::error_code(errno, std::system_category()));
}

} // namespace ou::http
