#pragma once

#include "UniqueFd.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ou {

enum class Interest : uint8_t { Read, Write };

struct ReadyEvent {
	int fd = -1;
	bool readable = false;
	bool writable = false;
};

// Waits on many sockets at once: epoll on Linux, kqueue on macOS/BSD, chosen at compile time.
// Level-triggered, so one read per wakeup is enough: what is left over is reported again next time.
class Poller {
public:
	Poller();

	// False if the kernel queue couldn't be created
	[[nodiscard]] bool valid() const noexcept { return queue_.valid(); }

	bool add(int fd, Interest interest);
	bool modify(int fd, Interest interest);
	bool remove(int fd);

	// Ready events, valid until the next wait()
	[[nodiscard]] std::span<const ReadyEvent> wait(std::chrono::milliseconds timeout);

private:
	static constexpr size_t kMaxEvents = 64;

	UniqueFd queue_;
	std::vector<ReadyEvent> ready_;
};

} // namespace ou
