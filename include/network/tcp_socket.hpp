#pragma once

#include "network/socket.hpp"
#include "network/net_platform.hpp"

namespace wowee {
namespace network {

class TCPSocket : public Socket {
public:
    TCPSocket();
    ~TCPSocket() override;

    bool connect(const std::string& host, uint16_t port) override;
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
};

} // namespace network
} // namespace wowee
