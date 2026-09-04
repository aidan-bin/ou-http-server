#pragma once

#include <unistd.h>
#include <utility>

namespace ou {

class UniqueFd {
public:
	UniqueFd() = default;
	explicit UniqueFd(int fd) noexcept : fd_(fd) {}

	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;

	UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, kInvalid)) {}

	UniqueFd &operator=(UniqueFd &&other) noexcept {
		if (this != &other) {
			reset();
			fd_ = std::exchange(other.fd_, kInvalid);
		}
		return *this;
	}

	~UniqueFd() { reset(); }

	[[nodiscard]] int get() const noexcept { return fd_; }
	[[nodiscard]] bool valid() const noexcept { return fd_ != kInvalid; }
	explicit operator bool() const noexcept { return valid(); }

	void reset() noexcept {
		if (valid()) {
			::close(fd_);
			fd_ = kInvalid;
		}
	}

	[[nodiscard]] int release() noexcept { return std::exchange(fd_, kInvalid); }

private:
	static constexpr int kInvalid = -1;

	int fd_ = kInvalid;
};

} // namespace ou
