#include "ou/http/Strings.h"

#include <algorithm>
#include <format>
#include <ranges>

namespace ou::http {

namespace {

	constexpr char sToLower(char ch) {
		if (ch >= 'A' && ch <= 'Z')
			return static_cast<char>(ch - 'A' + 'a');
		return ch;
	}

} // namespace

bool CaseInsensitiveLess::operator()(std::string_view lhs, std::string_view rhs) const noexcept {
	return std::ranges::lexicographical_compare(lhs, rhs, {}, sToLower, sToLower);
}

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept { return std::ranges::equal(lhs, rhs, {}, sToLower, sToLower); }

std::string percentDecode(std::string_view input, bool plusAsSpace) {
	std::string decoded;
	decoded.reserve(input.size());

	for (size_t i = 0; i < input.size(); ++i) {
		if (plusAsSpace && input[i] == '+') {
			decoded += ' ';
			continue;
		}

		if (input[i] == '%' && i + 2 < input.size()) {
			if (const std::optional<unsigned char> byte = parseNumber<unsigned char>(input.substr(i + 1, 2), 16)) {
				decoded += static_cast<char>(*byte);
				i += 2;
				continue;
			}
		}

		decoded += input[i];
	}

	return decoded;
}

std::string percentEncode(std::string_view input) {
	constexpr std::string_view kUnreserved = "-_.~/";

	std::string encoded;
	encoded.reserve(input.size());

	for (const unsigned char ch : input) {
		const bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
		                        || kUnreserved.find(static_cast<char>(ch)) != std::string_view::npos;
		if (unreserved) {
			encoded += static_cast<char>(ch);
			continue;
		}
		encoded += std::format("%{:02X}", static_cast<unsigned>(ch));
	}

	return encoded;
}

std::map<std::string, std::string> parseQuery(std::string_view query) {
	std::map<std::string, std::string> params;

	for (const auto &pair : std::views::split(query, '&')) {
		const std::string_view field(pair.begin(), pair.end());
		if (field.empty())
			continue;

		const size_t equals = field.find('=');
		if (equals == std::string_view::npos) {
			params[percentDecode(field)] = "";
			continue;
		}

		params[percentDecode(field.substr(0, equals))] = percentDecode(field.substr(equals + 1));
	}

	return params;
}

std::string_view trim(std::string_view str) {
	constexpr std::string_view kWhitespace = " \t\n\v\f\r";
	const size_t first = str.find_first_not_of(kWhitespace);
	if (first == std::string_view::npos)
		return {};
	return str.substr(first, str.find_last_not_of(kWhitespace) - first + 1);
}

} // namespace ou::http
