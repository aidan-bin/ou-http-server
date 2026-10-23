#pragma once

#include <algorithm>
#include <charconv>
#include <csignal>
#include <span>
#include <string_view>

#include <pthread.h>

namespace demo {

class Args {
public:
	Args(int argc, char **argv) : args_(std::span(argv, static_cast<size_t>(argc)).subspan(1)) {}

	[[nodiscard]] bool has(std::string_view flag) const { return std::ranges::contains(args_, flag); }

	[[nodiscard]] unsigned number(std::string_view prefix) const {
		unsigned value = 0;
		for (const std::string_view arg : args_) {
			if (arg.starts_with(prefix))
				(void)std::from_chars(std::to_address(arg.begin() + prefix.size()), std::to_address(arg.end()), value);
		}
		return value;
	}

private:
	std::span<char *> args_;
};

// Blocks SIGINT and SIGTERM in this thread and every thread started later, so only wait() receives them
class StopSignals {
public:
	StopSignals() {
		sigemptyset(&signals_);
		sigaddset(&signals_, SIGINT);
		sigaddset(&signals_, SIGTERM);
		pthread_sigmask(SIG_BLOCK, &signals_, nullptr);
	}

	void wait() const {
		int received = 0;
		sigwait(&signals_, &received);
	}

private:
	sigset_t signals_{};
};

} // namespace demo
