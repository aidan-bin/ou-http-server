#pragma once

#include "UniqueFd.h"

#include <array>
#include <boost/test/unit_test.hpp>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>

namespace ou::test {

inline std::optional<size_t> responseLength(std::string_view response) {
	const size_t headerEnd = response.find("\r\n\r\n");
	if (headerEnd == std::string_view::npos)
		return std::nullopt;

	size_t bodySize = 0;
	constexpr std::string_view kField = "\r\nContent-Length: ";
	if (const size_t field = response.find(kField); field < headerEnd)
		std::from_chars(response.data() + field + kField.size(), response.data() + headerEnd, bodySize);

	const size_t length = headerEnd + 4 + bodySize;
	return response.size() >= length ? std::optional(length) : std::nullopt;
}

std::string readResponse(std::invocable<std::span<char>> auto readChunk) {
	std::string response;
	std::array<char, 4096> chunk{};

	while (!responseLength(response)) {
		const ssize_t bytesRead = readChunk(std::span(chunk));
		if (bytesRead <= 0)
			break;
		response.append(chunk.data(), static_cast<size_t>(bytesRead));
	}

	return response;
}

class Client {
public:
	explicit Client(uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
		BOOST_REQUIRE(fd_.valid());

		const timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
		(void)::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		BOOST_REQUIRE_EQUAL(::connect(fd_.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
	}

	[[nodiscard]] int fd() const { return fd_.get(); }

	void send(std::string_view bytes) const { BOOST_REQUIRE_EQUAL(::send(fd_.get(), bytes.data(), bytes.size(), 0), ssize_t(bytes.size())); }

	[[nodiscard]] std::string receive() const {
		return readResponse([this](std::span<char> buffer) { return ::recv(fd_.get(), buffer.data(), buffer.size(), 0); });
	}

	void checkClosed() const {
		std::array<char, 64> chunk{};
		BOOST_CHECK_EQUAL(::recv(fd_.get(), chunk.data(), chunk.size(), 0), 0);
	}

private:
	UniqueFd fd_;
};

// Sends the pieces 20ms apart on a new connection; returns one response
inline std::string roundTrip(uint16_t port, std::initializer_list<std::string_view> pieces) {
	const Client client(port);

	for (const std::string_view piece : pieces) {
		client.send(piece);
		if (pieces.size() > 1)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	return client.receive();
}

inline std::string roundTrip(uint16_t port, std::string_view rawRequest) { return roundTrip(port, { rawRequest }); }

} // namespace ou::test
