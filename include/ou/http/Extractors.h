#pragma once

#include "ou/http/IntoResponse.h"
#include "ou/http/Request.h"
#include "ou/http/Response.h"
#include "ou/http/Strings.h"
#include "ou/http/Task.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ou::http {

// Handler parameter type: T::extract(request) yields a T or the error Response
template <typename T>
concept Extractor = requires(const Request &request) {
	{ T::extract(request) } -> std::same_as<std::expected<T, Response>>;
};

// String literal usable as a template argument, e.g., Query<"key">
template <size_t N>
struct FixedString {
	// NOLINTNEXTLINE(*-avoid-c-arrays) takes a literal by design
	constexpr FixedString(const char (&text)[N]) { std::ranges::copy(text, chars.begin()); }

	[[nodiscard]] constexpr std::string_view view() const { return { chars.data(), N - 1 }; }

	std::array<char, N> chars{};
};

template <size_t N>
struct PathKey {
	constexpr PathKey(size_t index) : index(index), name("") {}
	// NOLINTNEXTLINE(*-avoid-c-arrays) takes a literal by design
	constexpr PathKey(const char (&text)[N]) : name(text) {}

	[[nodiscard]] constexpr bool named() const { return N > 1; }

	size_t index = 0;
	FixedString<N> name;
};

// An index has no name, so N is 1
PathKey(size_t) -> PathKey<1>;

// Value types supported by Query, Header and Path
template <typename T>
concept ParamType = std::same_as<T, std::string> || std::integral<T>;

namespace detail {

	// nullopt if empty or malformed
	template <ParamType T>
	std::optional<T> parseParam(std::string_view text) {
		if (text.empty())
			return std::nullopt;
		if constexpr (std::integral<T>)
			return parseNumber<T>(text);
		else
			return std::string(text);
	}

} // namespace detail

// Required query parameter Name, parsed as T; 400 if missing, empty or malformed
template <FixedString Name, ParamType T = std::string>
struct Query {
	T value;

	[[nodiscard]] static std::expected<Query, Response> extract(const Request &request) {
		const auto params = parseQuery(request.query);
		const auto found = params.find(std::string(Name.view()));
		std::optional<T> parsed = found == params.end() ? std::nullopt : detail::parseParam<T>(found->second);

		if (!parsed)
			return std::unexpected(Response::plainText(400, std::format("Missing or bad {} parameter", Name.view())));
		return Query{ std::move(*parsed) };
	}
};

// Required request header Name, parsed as T; 400 if missing, empty or malformed
template <FixedString Name, ParamType T = std::string>
struct Header {
	T value;

	[[nodiscard]] static std::expected<Header, Response> extract(const Request &request) {
		std::optional<T> parsed = request.header(Name.view()).and_then(detail::parseParam<T>);

		if (!parsed)
			return std::unexpected(Response::plainText(400, std::format("Missing or bad {} header", Name.view())));
		return Header{ std::move(*parsed) };
	}
};

// Path<1> is a capture group, Path<"id"> an {id} segment; parsed as T, 400 if empty or malformed
template <PathKey Key, ParamType T = std::string>
struct Path {
	T value;

	[[nodiscard]] static std::expected<Path, Response> extract(const Request &request) {
		std::optional<T> parsed
				= (Key.named() ? request.pathParam(Key.name.view()) : request.pathParam(Key.index)).and_then(detail::parseParam<T>);

		if (!parsed)
			return std::unexpected(Response::plainText(
					400, std::format("Bad path parameter {}", Key.named() ? std::string(Key.name.view()) : std::to_string(Key.index))));
		return Path{ std::move(*parsed) };
	}
};

namespace detail {

	// The request itself, or an Extractor's value; a failure is stored in error (first one wins)
	template <typename P>
	P extractArg(const Request &request, std::optional<Response> &error) {
		if constexpr (std::same_as<P, const Request &>) {
			return request; // NOLINT(bugprone-return-const-ref-from-parameter) outlives the handler call
		} else {
			std::expected<P, Response> extracted = P::extract(request);
			if (extracted)
				return std::move(*extracted);
			if (!error)
				error = std::move(extracted).error();
			return P{};
		}
	}

	// What a handler returned, as a Response task: a value, or a task of one (which only needs a frame to convert)
	template <IntoResponse R>
	Task<Response> toResponseTask(R result) {
		return intoResponse(std::move(result));
	}

	inline Task<Response> toResponseTask(Task<Response> task) { return task; }

	template <IntoResponse U>
	Task<Response> toResponseTask(Task<U> task) {
		return then(std::move(task), [](U value) { return intoResponse(std::move(value)); });
	}

	template <typename F, typename R, typename... Ps>
	auto extracting(F handler, R (F::* /*callOperator*/)(Ps...) const) {
		static_assert(requires { toResponseTask(std::declval<R>()); }, "Handler must return a Response, a std::string, or a Task of one");
		// A reference to an Extractor would dangle once a coroutine handler suspends
		static_assert(((std::same_as<Ps, const Request &> || Extractor<Ps>) && ...),
		              "Handler parameters must be Extractors by value or const Request &");
		return [handler = std::move(handler)](const Request &request) -> Task<Response> {
			std::optional<Response> error;
			std::tuple<Ps...> args{ extractArg<Ps>(request, error)... }; // Braced init evaluates left to right
			if (error)
				return std::move(*error);
			return toResponseTask(std::apply(handler, std::move(args)));
		};
	}

} // namespace detail

// Adapts a lambda taking Extractors (and optionally const Request &) to a request handler, without a coroutine frame.
// It may return anything IntoResponse, or a Task of it.
template <typename F>
[[nodiscard]] auto extracting(F handler) {
	return detail::extracting(std::move(handler), &F::operator());
}

} // namespace ou::http
