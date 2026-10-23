#include "Poller.h"
#include "UniqueFd.h"

#include <array>
#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <span>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(transport)

BOOST_AUTO_TEST_CASE(test_unique_fd_closes_once) {
	ou::UniqueFd fd(::open("/dev/null", O_RDONLY));
	BOOST_REQUIRE(fd.valid());
	const int raw = fd.get();

	ou::UniqueFd moved(std::move(fd));
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

BOOST_AUTO_TEST_CASE(test_poller_readiness) {
	std::array<int, 2> pair{};
	BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()), 0);
	const ou::UniqueFd local(pair[0]);
	const ou::UniqueFd peer(pair[1]);

	ou::Poller poller;
	BOOST_REQUIRE(poller.valid());
	BOOST_REQUIRE(poller.add(local.get(), ou::Interest::Read));

	BOOST_CHECK(poller.wait(10ms).empty());

	BOOST_REQUIRE_EQUAL(::write(peer.get(), "x", 1), 1);
	std::span<const ou::ReadyEvent> ready = poller.wait(500ms);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK_EQUAL(ready[0].fd, local.get());
	BOOST_CHECK(ready[0].readable);

	// Switching interest hides the still-unread byte and reports the empty send buffer instead
	BOOST_REQUIRE(poller.modify(local.get(), ou::Interest::Write));
	ready = poller.wait(500ms);
	BOOST_REQUIRE_EQUAL(ready.size(), 1U);
	BOOST_CHECK(ready[0].writable);
	BOOST_CHECK(!ready[0].readable);

	BOOST_REQUIRE(poller.remove(local.get()));
	BOOST_CHECK(poller.wait(10ms).empty());
}

BOOST_AUTO_TEST_CASE(test_poller_wake) {
	ou::Poller poller;
	BOOST_REQUIRE(poller.enableWake());

	BOOST_CHECK(poller.wait(10ms).empty());

	const auto start = std::chrono::steady_clock::now();
	const std::jthread waker([&poller] {
		std::this_thread::sleep_for(50ms);
		poller.wake();
	});
	std::span<const ou::ReadyEvent> ready = poller.wait(5s);
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
