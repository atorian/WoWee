#include <catch_amalgamated.hpp>
#include "network/world_socket.hpp"
#include "core/logger.hpp"
#include <algorithm>

namespace wowee::network {
struct WorldSocketTestAccess {
    static void disableAsyncPump(WorldSocket& socket) { socket.useAsyncPump_ = false; }
    static void prepare(WorldSocket& socket, bool encrypted, bool legacy = false) {
        socket.connected = true;
        socket.useVanillaCrypt = legacy;
        socket.encryptionEnabled = encrypted;
        socket.decryptCipher.init({1, 2, 3, 4, 5});
        socket.decryptCipher.drop(1024);
    }
    static void feed(WorldSocket& socket, const std::vector<uint8_t>& bytes) {
        socket.receiveBuffer.insert(socket.receiveBuffer.end(), bytes.begin(), bytes.end());
        socket.tryParsePackets();
        socket.dispatchQueuedPackets();
    }
    static size_t decrypted(const WorldSocket& socket) { return socket.headerBytesDecrypted; }
};
}

using wowee::network::WorldSocket;
using wowee::network::WorldSocketTestAccess;
using wowee::network::Packet;

namespace {
std::vector<uint8_t> frame(uint32_t size, uint16_t opcode, bool extended,
                           wowee::auth::RC4* cipher = nullptr) {
    std::vector<uint8_t> bytes;
    if (extended) bytes.push_back(0x80 | (size >> 16));
    bytes.push_back(size >> 8);
    bytes.push_back(size);
    bytes.push_back(opcode);
    bytes.push_back(opcode >> 8);
    if (cipher) cipher->process(bytes.data(), bytes.size());
    for (uint32_t i = 0; i < size - 2; ++i) bytes.push_back(static_cast<uint8_t>(i * 17));
    return bytes;
}
}

TEST_CASE("WotLK extended encrypted headers survive every TCP header split", "[world_socket]") {
    const auto size = GENERATE(0x8603u, 0x12345u);
    const auto split = GENERATE(1u, 2u, 3u, 4u, 5u);
    WorldSocket socket;
    WorldSocketTestAccess::prepare(socket, true);
    wowee::auth::RC4 server;
    server.init({1, 2, 3, 4, 5});
    server.drop(1024);
    const auto large = frame(size, 0x1f6, true, &server);
    const auto next = frame(5, 0x1dd, false, &server);
    std::vector<Packet> packets;
    socket.setPacketCallback([&](const Packet& packet) { packets.push_back(packet); });
    WorldSocketTestAccess::feed(socket, {large.begin(), large.begin() + split});
    REQUIRE(packets.empty());
    WorldSocketTestAccess::feed(socket, {large.begin() + split, large.begin() + 105});
    REQUIRE(packets.empty());
    REQUIRE(WorldSocketTestAccess::decrypted(socket) == 5);
    // Force waiting for more payload twice: header must not be decrypted again.
    WorldSocketTestAccess::feed(socket, {});
    WorldSocketTestAccess::feed(socket, {large.begin() + 105, large.end()});
    REQUIRE(socket.isConnected());
    REQUIRE(packets.size() == 1);
    REQUIRE(packets[0].getOpcode() == 0x1f6);
    REQUIRE(packets[0].getSize() == size - 2);
    REQUIRE(std::equal(packets[0].getData().begin(), packets[0].getData().end(), large.begin() + 5));
    WorldSocketTestAccess::feed(socket, next);
    REQUIRE(packets.size() == 2);
    REQUIRE(packets[1].getOpcode() == 0x1dd);
    REQUIRE(packets[1].getData() == std::vector<uint8_t>{0, 17, 34});
}

TEST_CASE("Legacy high-bit size keeps the four-byte header", "[world_socket]") {
    WorldSocket socket;
    WorldSocketTestAccess::prepare(socket, false, true);
    size_t received = 0;
    socket.setPacketCallback([&](const Packet& packet) {
        REQUIRE(packet.getOpcode() == 0x1f6);
        REQUIRE(packet.getSize() == 0x8000);
        ++received;
    });
    WorldSocketTestAccess::feed(socket, frame(0x8002, 0x1f6, false));
    REQUIRE(socket.isConnected());
    REQUIRE(received == 1);
}

TEST_CASE("Invalid world header is rejected before allocating a payload", "[world_socket]") {
    WorldSocket socket;
    WorldSocketTestAccess::prepare(socket, false);
    const auto bytes = GENERATE(std::vector<uint8_t>{0, 1, 0, 0},
                                std::vector<uint8_t>{0xff, 0xff, 0xff, 0, 0});
    WorldSocketTestAccess::feed(socket, bytes);
    REQUIRE_FALSE(socket.isConnected());
}

TEST_CASE("Buffered packet bursts drain without another TCP receive", "[world_socket]") {
    WorldSocket socket;
    WorldSocketTestAccess::disableAsyncPump(socket);
    const socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(listener != INVALID_SOCK);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 1) == 0);
    socklen_t addressSize = sizeof(address);
    REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    REQUIRE(socket.connect("127.0.0.1", ntohs(address.sin_port)));
    const socket_t peer = ::accept(listener, nullptr, nullptr);
    REQUIRE(peer != INVALID_SOCK);
    std::vector<uint8_t> burst;
    for (uint16_t i = 0; i < 100; ++i) {
        const auto bytes = frame(2, i, false);
        burst.insert(burst.end(), bytes.begin(), bytes.end());
    }
    REQUIRE(wowee::net::portableSend(peer, burst.data(), burst.size()) == static_cast<ssize_t>(burst.size()));
    size_t received = 0;
    socket.setPacketCallback([&](const Packet& packet) {
        REQUIRE(packet.getOpcode() == received);
        REQUIRE(packet.getSize() == 0);
        ++received;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received < 100 && std::chrono::steady_clock::now() < deadline) {
        socket.update();
        std::this_thread::yield();
    }
    REQUIRE(socket.isConnected());
    REQUIRE(received == 100);
    socket.disconnect();
    wowee::net::closeSocket(peer);
    wowee::net::closeSocket(listener);
}
