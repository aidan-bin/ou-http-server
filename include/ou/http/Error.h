#pragma once

#include <format>
#include <string>
#include <system_error>

namespace ou::http {

// A setup step that failed
struct Error {
	std::string step;
	std::error_code code; // Empty when the step has no errno; its reason is then part of step

	[[nodiscard]] std::string message() const {
		return code ? std::format("Failed to {}: {}", step, code.message()) : std::format("Failed to {}", step);
	}
};

} // namespace ou::http
