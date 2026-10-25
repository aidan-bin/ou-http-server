#include "ReadStream.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

using namespace ou::test;

// First byte picks the read size; N-byte reads must parse exactly like one whole read
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size == 0 || size - 1 > kReadStreamMaxBytes)
		return 0;

	const size_t chunkSize = data[0] + 1;
	const std::string_view stream(reinterpret_cast<const char *>(data + 1), size - 1);

	if (readStream(stream, chunkSize) != readStream(stream, stream.size()))
		std::abort();
	return 0;
}
