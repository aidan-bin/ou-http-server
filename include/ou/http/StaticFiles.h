#pragma once

#include "ou/http/Error.h"
#include "ou/http/HttpTypes.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <utility>

namespace ou::http {

// Serves files under a directory; attach it with Router::fallback()
class StaticFiles {
public:
	struct Config {
		std::filesystem::path directory;
		bool enableDirectoryIndexing = false;

		// Loads files into memory at creation (up to this many bytes total). 0 disables.
		// Files that don't fit are served from disk. Changes on disk afterwards are not seen.
		size_t preloadBytes = 0;

		// Skips resolving symlinks on each disk request. ".." is still rejected, but a symlink inside the
		// directory can point anywhere.
		bool skipSymlinkCheck = false;
	};

	// Fails if directory is not a directory
	[[nodiscard]] static std::expected<StaticFiles, Error> create(Config config);

	// Serves the file the path names, a listing of the directory it names, or an error status
	[[nodiscard]] Response operator()(const Request &request) const;

private:
	struct State; // Shared and immutable, so a copy (a Handler is copied into the Router) is cheap

	explicit StaticFiles(std::shared_ptr<const State> state) : state_(std::move(state)) {}

	std::shared_ptr<const State> state_;
};

} // namespace ou::http
