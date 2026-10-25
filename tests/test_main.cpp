// The Boost.Test runner, compiled once
#define BOOST_TEST_MODULE OuHttpServerTest
#include <boost/test/included/unit_test.hpp>

#include <csignal>

// Rejected TLS handshakes write to a peer that already closed
struct IgnoreSigpipe {
	IgnoreSigpipe() { (void)std::signal(SIGPIPE, SIG_IGN); }
};

BOOST_TEST_GLOBAL_FIXTURE(IgnoreSigpipe);
