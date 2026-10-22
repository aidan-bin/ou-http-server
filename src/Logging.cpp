#include "ou/http/Logging.h"

#include <cstdio>
#include <iostream>
#include <mutex>
#include <utility>

namespace ou {

namespace {

	struct State {
		std::mutex mutex;
		LogSink sink;
	};

	State &sState() {
		static State state;
		return state;
	}

} // namespace

void setLogSink(LogSink sink) {
	State &state = sState();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.sink = std::move(sink);
}

void detail::emit(LogLevel level, const std::string &line) noexcept {
	try {
		State &state = sState();
		const std::lock_guard<std::mutex> lock(state.mutex);
		if (state.sink) {
			state.sink(line);
			return;
		}
		std::cout << line << '\n';
		if (level != LogLevel::Info)
			std::cout.flush();
	} catch (...) {
		(void)std::fputs("[ERROR] Failed to write a log line\n", stderr);
	}
}

} // namespace ou
