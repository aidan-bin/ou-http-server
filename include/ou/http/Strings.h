#pragma once

#include <charconv>
#include <concepts>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ou::http {

[[nodiscard]] bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept;

[[nodiscard]] std::string_view trim(std::string_view str);

struct CaseInsensitiveLess {
	using is_transparent = void;
	[[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
};

// Parses all of text as a number; nullopt if anything is left over
template <std::integral T>
[[nodiscard]] std::optional<T> parseNumber(std::string_view text, int base = 10) {
	T value{};
	const char *end = std::to_address(text.end());
	const auto [stopped, ec] = std::from_chars(std::to_address(text.begin()), end, value, base);
	if (ec != std::errc{} || stopped != end)
		return std::nullopt;
	return value;
}

// Decodes %XX escapes; invalid escapes are left verbatim. '+' means space only in query/form data
[[nodiscard]] std::string percentDecode(std::string_view input, bool plusAsSpace = true);

// Escapes everything outside the unreserved set, leaving '/' intact so a path stays a path
[[nodiscard]] std::string percentEncode(std::string_view input);

// Splits "a=1&b=2" and percent-decodes each name and value; later duplicates win
[[nodiscard]] std::map<std::string, std::string> parseQuery(std::string_view query);

} // namespace ou::http
