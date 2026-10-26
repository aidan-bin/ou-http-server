#include "ou/http/Logging.h"

#include <cstdio>
#include <iostream>
#include <mutex>
#include <utility>

namespace ou::http::log {

namespace {

	struct State {
		std::mutex mutex;
		Sink sink;
	};

	State &sState() {
		static State state;
		return state;
	}

} // namespace

void setSink(Sink sink) {
	State &state = sState();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.sink = std::move(sink);
}

void detail::emit(Level level, const std::string &line) noexcept {
	try {
		State &state = sState();
		const std::lock_guard<std::mutex> lock(state.mutex);
		if (state.sink) {
			state.sink(line);
			return;
		}
		std::cout << line << '\n';
		if (level != Level::Info)
			std::cout.flush();
	} catch (...) {
		(void)std::fputs("[ERROR] Failed to write a log line\n", stderr);
	}
}

} // namespace ou::http::log
