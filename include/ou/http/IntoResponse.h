#pragma once

#include "ou/http/HttpTypes.h"

#include <concepts>
#include <expected>
#include <string>
#include <utility>

namespace ou::http {

[[nodiscard]] inline Response intoResponse(Response response) { return response; }

[[nodiscard]] inline Response intoResponse(std::string body) { return Response::plainText(200, std::move(body)); }

// Either side converts on its own, e.g., an expected<std::string, Response> answers an error as itself
template <typename T, typename E>
	requires requires(T value, E error) {
		intoResponse(std::move(value));
		intoResponse(std::move(error));
	}
[[nodiscard]] Response intoResponse(std::expected<T, E> result) {
	return result ? intoResponse(*std::move(result)) : intoResponse(std::move(result).error());
}

// A type a handler may return
template <typename T>
concept IntoResponse = requires(T value) {
	{ intoResponse(std::move(value)) } -> std::same_as<Response>;
};

} // namespace ou::http
