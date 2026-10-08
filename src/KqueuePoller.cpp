#include "Poller.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <span>

#include <sys/event.h>
#include <sys/time.h>

namespace ou {

namespace {

	bool sApply(int queue, int fd, int16_t filter, uint16_t flags) {
		struct kevent change{};
		EV_SET(&change, static_cast<uintptr_t>(fd), filter, flags, 0, 0, nullptr);
		return kevent(queue, &change, 1, nullptr, 0, nullptr) == 0;
	}

	// EVFILT_USER idents are their own namespace, so 0 can't collide with a socket
	bool sApplyWake(int queue, uint16_t flags, uint32_t fflags) {
		struct kevent change{};
		EV_SET(&change, 0, EVFILT_USER, flags, fflags, 0, nullptr);
		return kevent(queue, &change, 1, nullptr, 0, nullptr) == 0;
	}

} // namespace

Poller::Poller() : queue_(kqueue()) {}

bool Poller::add(int fd, Interest interest) {
	const bool wantsRead = interest == Interest::Read;
	const uint16_t readFlags = EV_ADD | (wantsRead ? EV_ENABLE : EV_DISABLE);
	const uint16_t writeFlags = EV_ADD | (wantsRead ? EV_DISABLE : EV_ENABLE);

	return sApply(queue_.get(), fd, EVFILT_READ, readFlags) && sApply(queue_.get(), fd, EVFILT_WRITE, writeFlags);
}

bool Poller::modify(int fd, Interest interest) {
	// Both filters are registered up front and one is left disabled, so just add() again
	return add(fd, interest);
}

bool Poller::remove(int fd) {
	// ENOENT is not a failure here: a filter that was never registered is already gone
	(void)sApply(queue_.get(), fd, EVFILT_READ, EV_DELETE);
	(void)sApply(queue_.get(), fd, EVFILT_WRITE, EV_DELETE);
	return true;
}

bool Poller::enableWake() {
	// EV_CLEAR resets the trigger once reported, so wakes between waits collapse into one event
	return sApplyWake(queue_.get(), EV_ADD | EV_CLEAR, 0);
}

void Poller::wake() { (void)sApplyWake(queue_.get(), 0, NOTE_TRIGGER); }

std::span<const ReadyEvent> Poller::wait(std::chrono::milliseconds timeout) {
	const timespec deadline{ .tv_sec = static_cast<time_t>(timeout.count() / 1000),
													 .tv_nsec = static_cast<long>(timeout.count() % 1000) * 1000000 };
	std::array<struct kevent, kMaxEvents> events{};

	int count = 0;
	while (true) {
		count = kevent(queue_.get(), nullptr, 0, events.data(), static_cast<int>(events.size()), &deadline);
		if (count >= 0 || errno != EINTR)
			break;
	}

	ready_.clear();
	for (const struct kevent &event : std::span(events).first(static_cast<size_t>(std::max(count, 0)))) {
		if (event.filter == EVFILT_USER) {
			ready_.push_back({ .woken = true });
			continue;
		}

		const int fd = static_cast<int>(event.ident);

		// One call can report both filters for an fd; coalesce so the two backends look identical
		const auto existing = std::ranges::find(ready_, fd, &ReadyEvent::fd);
		ReadyEvent &ready = existing != ready_.end() ? *existing : ready_.emplace_back(ReadyEvent{ .fd = fd });

		ready.readable = ready.readable || event.filter == EVFILT_READ || (event.flags & EV_EOF) != 0;
		ready.writable = ready.writable || (event.filter == EVFILT_WRITE && (event.flags & EV_EOF) == 0);
	}

	return ready_;
}

} // namespace ou

#endif // kqueue platforms
