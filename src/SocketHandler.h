#pragma once

#include <string>
#include <sys/socket.h>
#include <unistd.h>

class SocketHandler {
public:
	virtual ~SocketHandler() = default;
	virtual bool acceptConnection(int clientSocket) = 0;
	virtual ssize_t read(int clientSocket, char *buffer, size_t size) = 0;
	virtual ssize_t write(int clientSocket, const std::string &data) = 0;
	virtual void closeConnection(int clientSocket) = 0;
};

class PlainSocketHandler : public SocketHandler {
public:
	bool acceptConnection(int clientSocket) override final {
		(void)clientSocket;
		return true;
	}
	ssize_t read(int clientSocket, char *buffer, size_t size) override final { return ::read(clientSocket, buffer, size); }
	ssize_t write(int clientSocket, const std::string &data) override final { return ::send(clientSocket, data.c_str(), data.size(), 0); }
	void closeConnection(int clientSocket) override final { ::close(clientSocket); }
};
