#include "Poller.h"

#ifdef __linux__

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <span>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace ou::http {

namespace {

	bool sControl(int queue, int op, int fd, Interest interest) {
		epoll_event event{};
		event.events = interest == Interest::Write ? EPOLLOUT : EPOLLIN;
		event.data.fd = fd;
		return epoll_ctl(queue, op, fd, &event) == 0;
	}

} // namespace

Poller::Poller() : queue_(epoll_create1(EPOLL_CLOEXEC)) {}

bool Poller::add(int fd, Interest interest) { return sControl(queue_.get(), EPOLL_CTL_ADD, fd, interest); }

bool Poller::modify(int fd, Interest interest) { return sControl(queue_.get(), EPOLL_CTL_MOD, fd, interest); }

bool Poller::remove(int fd) { return epoll_ctl(queue_.get(), EPOLL_CTL_DEL, fd, nullptr) == 0; }

bool Poller::enableWake() {
	wakeFd_ = UniqueFd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
	return wakeFd_.valid() && sControl(queue_.get(), EPOLL_CTL_ADD, wakeFd_.get(), Interest::Read);
}

void Poller::wake() {
	const uint64_t one = 1;
	while (::write(wakeFd_.get(), &one, sizeof(one)) < 0 && errno == EINTR) {
	}
}

std::span<const ReadyEvent> Poller::wait(std::chrono::milliseconds timeout) {
	std::array<epoll_event, kMaxEvents> events{};

	int count = 0;
	while (true) {
		count = epoll_wait(queue_.get(), events.data(), static_cast<int>(events.size()), static_cast<int>(timeout.count()));
		if (count >= 0 || errno != EINTR)
			break;
	}

	ready_.clear();

	for (const epoll_event &event : std::span(events).first(static_cast<size_t>(std::max(count, 0)))) {
		if (event.data.fd == wakeFd_.get()) {
			uint64_t wakes = 0;
			[[maybe_unused]] const ssize_t drained = ::read(wakeFd_.get(), &wakes, sizeof(wakes)); // GCC ignores a (void) cast here
			ready_.push_back({ .woken = true });
			continue;
		}

		ready_.push_back({ .fd = event.data.fd });
	}

	return ready_;
}

} // namespace ou::http

#endif // __linux__
