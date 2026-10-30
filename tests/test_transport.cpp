#include "Listener.h"
#include "Poller.h"
#include "UniqueFd.h"

#include <array>
#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <expected>
#include <span>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace ou::http;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(transport)

BOOST_AUTO_TEST_CASE(test_unique_fd_closes_once) {
	UniqueFd fd(::open("/dev/null", O_RDONLY));
	BOOST_REQUIRE(fd.valid());
	const int raw = fd.get();

	UniqueFd moved(std::move(fd));
	BOOST_CHECK(moved.valid());
	// NOLINTNEXTLINE(bugprone-use-after-move) checking the moved-from state is the point
	BOOST_CHECK(!fd.valid());
	BOOST_CHECK_EQUAL(moved.get(), raw);

	moved.reset();
	BOOST_CHECK(!moved.valid());
	moved.reset(); // Idempotent

	errno = 0;
	BOOST_CHECK_EQUAL(::fcntl(raw, F_GETFD), -1);
	BOOST_CHECK_EQUAL(errno, EBADF);
}

BOOST_AUTO_TEST_CASE(test_accept_tells_transient_errors_from_persistent_ones) {
	const std::expected<Listener, Error> listener = openListener(0);
	BOOST_REQUIRE(listener);
	sockaddr_in clientAddr{};

	const auto none = acceptClient(listener->socket.get(), clientAddr);
	BOOST_REQUIRE(none);
	BOOST_CHECK(!none->valid());

	const UniqueFd notListening(::socket(AF_INET, SOCK_STREAM, 0));
	BOOST_CHECK(!acceptClient(notListening.get(), clientAddr));
}

BOOST_AUTO_TEST_CASE(test_poller_readiness) {
	std::array<int, 2> pair{};
	BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()), 0);
	const UniqueFd local(pair[0]);
	const UniqueFd peer(pair[1]);

	Poller poller;
	BOOST_REQUIRE(poller.valid());
	BOOST_REQUIRE(poller.add(local.get(), Interest::Read));

	BOOST_CHECK(poller.wait(10ms).empty());

	BOOST_REQUIRE_EQUAL(::write(peer.get(), "x", 1), 1);
	std::span<const ReadyEvent> ready = poller.wait(500ms);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK_EQUAL(ready[0].fd, local.get());

	// Write interest reports the empty send buffer, then nothing once it is full, despite the unread byte
	BOOST_REQUIRE(poller.modify(local.get(), Interest::Write));
	ready = poller.wait(500ms);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK_EQUAL(ready[0].fd, local.get());

	BOOST_REQUIRE(setNonBlocking(local.get(), true));
	const std::array<char, 4096> filler{};
	while (::write(local.get(), filler.data(), filler.size()) > 0) {
	}
	BOOST_CHECK(poller.wait(10ms).empty());

	BOOST_REQUIRE(poller.remove(local.get()));
	BOOST_CHECK(poller.wait(10ms).empty());
}

BOOST_AUTO_TEST_CASE(test_poller_wake) {
	Poller poller;
	BOOST_REQUIRE(poller.enableWake());

	BOOST_CHECK(poller.wait(10ms).empty());

	const auto start = std::chrono::steady_clock::now();
	const std::jthread waker([&poller] {
		std::this_thread::sleep_for(50ms);
		poller.wake();
	});
	std::span<const ReadyEvent> ready = poller.wait(5s);
	BOOST_CHECK_LT(std::chrono::steady_clock::now() - start, 2s);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK(ready[0].woken);

	poller.wake();
	poller.wake();
	poller.wake();
	ready = poller.wait(500ms);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK(ready[0].woken);
	BOOST_CHECK(poller.wait(10ms).empty());
}

BOOST_AUTO_TEST_SUITE_END()
