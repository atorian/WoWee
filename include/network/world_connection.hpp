#pragma once

// The world server connection as the game sees it: connect, send, receive,
// start the header cipher, close. WorldSocket is the one over TCP; a test
// hands the game one that records what it sent and plays it packets.

#include "network/async_connect.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee::network {

class Packet;

class WorldConnection {
public:
    virtual ~WorldConnection() = default;

    /// Start connecting; pollConnect says when it is done.
    virtual void beginConnect(const std::string& host, uint16_t port, int timeoutSeconds = 10) = 0;
    virtual net::AsyncConnect::Status pollConnect() = 0;
    /// Why the last connect failed, for the log and the login screen.
    [[nodiscard]] virtual std::string connectError() const = 0;

    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;

    virtual void send(const Packet& packet) = 0;
    /// Deliver what has arrived, to the packet callback. Once a frame.
    virtual void update() = 0;
    virtual void setPacketCallback(std::function<void(const Packet&)> callback) = 0;

    /// Encrypt the headers from here on, with the session key from the
    /// login server. After CMSG_AUTH_SESSION.
    virtual void initEncryption(const std::vector<uint8_t>& sessionKey, uint32_t build) = 0;
    /// Log every packet for a while, with a reason, to diagnose a stall.
    virtual void tracePacketsFor(std::chrono::milliseconds duration, const std::string& reason) = 0;
};

} // namespace wowee::network
