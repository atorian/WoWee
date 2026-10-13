#pragma once

#include "network/socket.hpp"
#include "network/net_platform.hpp"
#include "network/async_connect.hpp"

namespace wowee {
namespace network {

class TCPSocket : public Socket {
public:
    TCPSocket();
    ~TCPSocket() override;

    bool connect(const std::string& host, uint16_t port) override;
    /// Connects off the main thread; pollConnect() says when it is done and,
    /// on success, leaves the socket connected as connect() would.
    void beginConnect(const std::string& host, uint16_t port, int timeoutSeconds = 5);
    net::AsyncConnect::Status pollConnect();
    [[nodiscard]] const std::string& connectError() const { return connectError_; }
    void disconnect() override;
    [[nodiscard]] bool isConnected() const override { return connected; }

    void send(const Packet& packet) override;
    void update() override;

private:
    void tryParsePackets();
    // Bytes the auth packet starting at data occupies, or 0 when more must arrive.
    size_t getExpectedPacketSize(const uint8_t* data, size_t size);

    socket_t sockfd = INVALID_SOCK;
    bool connected = false;
    std::vector<uint8_t> receiveBuffer;
    net::AsyncConnect pendingConnect_;
    std::string connectHost_;
    uint16_t connectPort_ = 0;
    std::string connectError_;
};

} // namespace network
} // namespace wowee
