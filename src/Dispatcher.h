#pragma once

#include "ou/http/Request.h"
#include "ou/http/Response.h"
#include "ou/http/Router.h"
#include "ou/http/Task.h"

#include <netinet/in.h>

namespace ou::http {

// Turns a parsed request into a finished response, and answers requests that never parsed.
// Immutable, so every loop thread shares one.
class Dispatcher {
public:
	Dispatcher(bool keepAlive, bool logRequests, Router router);

	// Routes the request, then sets keep-alive and logs it; request must outlive the task
	[[nodiscard]] Task<Response> serve(Request &request, const sockaddr_in &clientAddr) const;

	// Answers a request that never parsed with status, through every middleware's after()
	[[nodiscard]] Response reject(int status, const sockaddr_in &clientAddr) const;

private:
	bool keepAlive_;
	bool logRequests_;
	Router router_;
};

} // namespace ou::http
