#include "Listener.h"

#include <array>
#include <cerrno>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ou::http {

namespace {

	using Opened = std::expected<UniqueFd, Error>;

	// Reads errno, so call it before anything (like closing the socket) can overwrite it
	Error sErrnoError(std::string_view step) { return { .step = std::string(step), .code = std::error_code(errno, std::system_category()) }; }

	// Passes the socket on if the step's call succeeded
	Opened sCheck(UniqueFd socket, int result, std::string_view step) {
		if (result < 0)
			return std::unexpected(sErrnoError(step));
		return socket;
	}

	Opened sCreate() {
		UniqueFd created(::socket(AF_INET, SOCK_STREAM, 0));
		if (!created)
			return std::unexpected(sErrnoError("create socket"));
		return created;
	}

	// Lets a restarted server rebind while the old socket's connections sit in TIME_WAIT
	Opened sReuseAddress(UniqueFd socket) {
		const int enable = 1;
		const int result = setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
		return sCheck(std::move(socket), result, "set SO_REUSEADDR");
	}

	Opened sBind(uint16_t port, UniqueFd socket) {
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(port);
		const int result = bind(socket.get(), reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
		return sCheck(std::move(socket), result, "bind socket");
	}

	Opened sListen(UniqueFd socket) {
		const int result = listen(socket.get(), SOMAXCONN);
		return sCheck(std::move(socket), result, "listen on socket");
	}

	Opened sNonBlocking(UniqueFd socket) {
		const int result = setNonBlocking(socket.get(), true) ? 0 : -1;
		return sCheck(std::move(socket), result, "set socket non-blocking");
	}

	std::expected<Listener, Error> sWithBoundPort(UniqueFd socket) {
		sockaddr_in bound{};
		socklen_t length = sizeof(bound);
		if (getsockname(socket.get(), reinterpret_cast<struct sockaddr *>(&bound), &length) < 0)
			return std::unexpected(sErrnoError("read the bound port"));
		return Listener{ .socket = std::move(socket), .port = ntohs(bound.sin_port) };
	}

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
	return sCreate()
	    .and_then(sReuseAddress)
	    .and_then(std::bind_front(sBind, port))
	    .and_then(sListen)
	    .and_then(sNonBlocking)
	    .and_then(sWithBoundPort);
}

std::expected<UniqueFd, std::error_code> acceptClient(int listenSocket, sockaddr_in &clientAddr) {
	socklen_t length = sizeof(clientAddr);
	UniqueFd client(::accept(listenSocket, reinterpret_cast<struct sockaddr *>(&clientAddr), &length));
	if (client || sIsTransientAcceptError(errno))
		return client;
	return std::unexpected(std::error_code(errno, std::system_category()));
}

} // namespace ou::http
