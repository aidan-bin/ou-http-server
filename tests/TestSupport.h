#pragma once

#include "ou/http/HttpTypes.h"
#include "ou/http/Server.h"
#include "ou/http/StaticFiles.h"

#include <array>
#include <boost/test/unit_test.hpp>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

namespace ou::http {
inline std::ostream &operator<<(std::ostream &os, Method method) { return os << methodToString(method); }

inline std::ostream &operator<<(std::ostream &os, Server::Model model) {
	return os << (model == Server::Model::EventLoop ? "EventLoop" : "ThreadPerConnection");
}
} // namespace ou::http

namespace ou::test {

using http::Method;
using http::Request;
using http::Response;
using http::Router;
using http::Server;
using http::StaticFiles;
using http::Task;

inline constexpr std::array kModels{ Server::Model::ThreadPerConnection, Server::Model::EventLoop };

// Named per process and test case, so parallel suites don't collide
class TempDir {
public:
	TempDir()
			: path_(std::filesystem::temp_directory_path()
	            / std::format("ou_http_server_test_{}_{}", ::getpid(), boost::unit_test::framework::current_test_case().p_id.get())) {
		std::filesystem::remove_all(path_);
		std::filesystem::create_directories(path_);
	}
	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;
	TempDir(TempDir &&) = delete;
	TempDir &operator=(TempDir &&) = delete;
	~TempDir() {
		std::error_code ignored;
		std::filesystem::remove_all(path_, ignored);
	}

	[[nodiscard]] const std::filesystem::path &path() const { return path_; }

	void write(const std::filesystem::path &name, std::string_view content) const { std::ofstream(path_ / name) << content; }

private:
	std::filesystem::path path_;
};

inline std::vector<std::string> lines(const std::filesystem::path &path) {
	std::ifstream file(path);
	std::vector<std::string> result;
	for (std::string line; std::getline(file, line);)
		result.push_back(line);
	return result;
}

inline std::expected<Request, int> parseHead(std::string_view raw) { return Request::parse(raw.substr(0, raw.find("\r\n\r\n"))); }

inline Request parse(std::string_view raw) {
	std::expected<Request, int> parsed = parseHead(raw);
	BOOST_REQUIRE(parsed.has_value());
	return std::move(*parsed);
}

// From 127.0.0.1, so AccessLog logs it
inline Request makeRequest(const std::string &path, Method method = Method::GET) {
	Request request;
	request.method = method;
	request.path = path;

	sockaddr_in clientAddr{};
	clientAddr.sin_family = AF_INET;
	clientAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	request.clientAddr = clientAddr;

	return request;
}

inline Server::Config makeConfig(Server::Model model = Server::Config{}.model) {
	Server::Config config;
	config.port = 0;
	config.threadCount = 2;
	config.model = model;
	return config;
}

inline Server startServer(const Server::Config &config, Router router = {}) {
	std::expected<Server, http::Error> server = Server::start(config, std::move(router));
	BOOST_REQUIRE(server);
	return std::move(*server);
}

inline StaticFiles openFiles(StaticFiles::Config config) {
	std::expected<StaticFiles, http::Error> files = StaticFiles::create(std::move(config));
	BOOST_REQUIRE(files);
	return std::move(*files);
}

// Runs a request through a router without sockets
inline Response respond(const Router &router, Request request) {
	Task<Response> handled = router.handle(request);
	return handled.result();
}

struct ServingRoot {
	TempDir root;

	[[nodiscard]] Router files() const { return Router().fallback(openFiles({ .directory = root.path() })); }
};

} // namespace ou::test
