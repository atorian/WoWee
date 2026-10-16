// Logging in again with the last session's key: the login server's reconnect
// handshake (RECONNECT_CHALLENGE, RECONNECT_PROOF), driven through the real
// AuthHandler and its TCP socket against a login server that is the test,
// listening on the loopback interface. What the handler sends is read off the
// wire, and what the server answers decides what the handler reports.
#include <catch_amalgamated.hpp>

#include "auth/auth_handler.hpp"
#include "auth/crypto.hpp"
#include "network/net_platform.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using wowee::auth::AuthHandler;
using wowee::auth::AuthState;
namespace net = wowee::net;
using Bytes = std::vector<uint8_t>;

namespace {

/// One login server on 127.0.0.1, one client at a time, no threads: the test
/// takes turns with the handler, as its frame would.
class LoopbackLoginServer {
public:
    LoopbackLoginServer() {
        net::ensureInit();
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener_ != INVALID_SOCK);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listener_, 1) == 0);
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        net::setNonBlocking(listener_);
    }
    ~LoopbackLoginServer() {
        dropClient();
        if (listener_ != INVALID_SOCK) net::closeSocket(listener_);
    }

    [[nodiscard]] uint16_t port() const { return port_; }

    /// Takes the handler's connection when it has arrived.
    void poll() {
        if (client_ == INVALID_SOCK) {
            client_ = ::accept(listener_, nullptr, nullptr);
            if (client_ != INVALID_SOCK) net::setNonBlocking(client_);
            return;
        }
        uint8_t buffer[512];
        for (;;) {
            const auto n = net::portableRecv(client_, buffer, sizeof(buffer));
            if (n <= 0) break;
            received_.insert(received_.end(), buffer, buffer + n);
        }
    }
    void send(const Bytes& bytes) {
        REQUIRE(client_ != INVALID_SOCK);
        REQUIRE(net::portableSend(client_, bytes.data(), bytes.size()) ==
                static_cast<ssize_t>(bytes.size()));
    }
    void dropClient() {
        if (client_ != INVALID_SOCK) net::closeSocket(client_);
        client_ = INVALID_SOCK;
    }
    /// What the client sent and the server has not taken yet.
    Bytes& received() { return received_; }
    /// Takes the first `n` bytes received.
    Bytes take(size_t n) {
        Bytes out(received_.begin(), received_.begin() + static_cast<std::ptrdiff_t>(n));
        received_.erase(received_.begin(), received_.begin() + static_cast<std::ptrdiff_t>(n));
        return out;
    }

private:
    socket_t listener_ = INVALID_SOCK;
    socket_t client_ = INVALID_SOCK;
    uint16_t port_ = 0;
    Bytes received_;
};

/// Frames of the handler and turns of the server until `done`, or fail after
/// five seconds of wall time.
void runUntil(AuthHandler& auth, LoopbackLoginServer& server, const std::function<bool()>& done,
              const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) FAIL("timed out waiting for " << what);
        auth.update(1.0f / 60.0f);
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

const Bytes kSessionKey = [] {
    Bytes key(40);
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(0x30 + i);
    return key;
}();

const Bytes kChallenge = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                          0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x01};

struct Connected {
    LoopbackLoginServer server;
    AuthHandler auth;
    std::vector<std::string> failures;
    std::vector<Bytes> successes;

    Connected() {
        auth.setOnFailure([this](const std::string& why) { failures.push_back(why); });
        auth.setOnSuccess([this](const Bytes& key) { successes.push_back(key); });
        auth.beginConnect("127.0.0.1", server.port());
        runUntil(auth, server, [this] { return auth.getState() == AuthState::CONNECTED; },
                 "the connection");
    }

    /// The handler names the account: the logon challenge's layout under the
    /// reconnect command. Answers how many bytes it was.
    size_t readChallenge() {
        runUntil(auth, server, [this] { return server.received().size() >= 4; }, "the challenge");
        const auto& r = server.received();
        const size_t size = 4 + (r[2] | (r[3] << 8));
        runUntil(auth, server, [&] { return server.received().size() >= size; }, "the whole challenge");
        return size;
    }
};

Bytes challengeAnswer(uint8_t error) {
    Bytes b = {0x02, error};
    if (error == 0) {
        b.insert(b.end(), kChallenge.begin(), kChallenge.end());
        b.insert(b.end(), 16, 0x00);  // version challenge
    }
    return b;
}

} // namespace

TEST_CASE("a reconnect proves the last session's key and is in, with that key, without a password",
          "[auth][reconnect]") {
    Connected c;
    c.auth.reconnect("Tester", kSessionKey);

    // RECONNECT_CHALLENGE, naming the account as a logon challenge does.
    const Bytes challenge = c.server.take(c.readChallenge());
    CHECK(challenge[0] == 0x02);
    const std::string account(challenge.end() - 6, challenge.end());
    CHECK(account == "TESTER");
    CHECK(challenge[challenge.size() - 7] == 6);  // the name's length before it

    c.server.send(challengeAnswer(0x00));
    runUntil(c.auth, c.server, [&] { return c.server.received().size() >= 58; }, "the proof");
    const Bytes proof = c.server.take(58);
    CHECK(proof[0] == 0x03);
    // R2 = SHA1(account, R1, the server's challenge, the session key).
    const Bytes r1(proof.begin() + 1, proof.begin() + 17);
    Bytes hashed(account.begin(), account.end());
    hashed.insert(hashed.end(), r1.begin(), r1.end());
    hashed.insert(hashed.end(), kChallenge.begin(), kChallenge.end());
    hashed.insert(hashed.end(), kSessionKey.begin(), kSessionKey.end());
    const Bytes r2(proof.begin() + 17, proof.begin() + 37);
    CHECK(r2 == wowee::auth::Crypto::sha1(hashed));
    CHECK(proof[57] == 0);  // no security keys

    // The server accepts: the handler is in, with the same key, and the realm
    // list can be asked for on this connection.
    c.server.send({0x03, 0x00, 0x00, 0x00});
    runUntil(c.auth, c.server, [&] { return c.auth.getState() == AuthState::AUTHENTICATED; },
             "the handler to be in");
    CHECK(c.auth.getSessionKey() == kSessionKey);
    REQUIRE(c.successes.size() == 1);
    CHECK(c.successes[0] == kSessionKey);
    CHECK(c.failures.empty());

    c.auth.requestRealmList();
    runUntil(c.auth, c.server, [&] { return !c.server.received().empty(); }, "the realm list request");
    CHECK(c.server.received()[0] == 0x10);
}

TEST_CASE("a login server that holds no session for the key refuses the reconnect, and says so",
          "[auth][reconnect]") {
    Connected c;
    c.auth.reconnect("Tester", kSessionKey);
    c.server.take(c.readChallenge());

    SECTION("an error answer to the challenge") {
        c.server.send(challengeAnswer(0x04));  // unknown account
        runUntil(c.auth, c.server, [&] { return c.auth.getState() == AuthState::FAILED; }, "the failure");
        // No proof went out for a challenge that was refused.
        c.server.poll();
        CHECK(c.server.received().empty());
    }
    SECTION("an error answer to the proof") {
        c.server.send(challengeAnswer(0x00));
        runUntil(c.auth, c.server, [&] { return c.server.received().size() >= 58; }, "the proof");
        c.server.send({0x03, 0x0E});
        runUntil(c.auth, c.server, [&] { return c.auth.getState() == AuthState::FAILED; }, "the failure");
    }
    SECTION("the connection closed on the proof, as TrinityCore answers a key it does not hold") {
        c.server.send(challengeAnswer(0x00));
        runUntil(c.auth, c.server, [&] { return c.server.received().size() >= 58; }, "the proof");
        c.server.dropClient();
        runUntil(c.auth, c.server, [&] { return c.auth.getState() == AuthState::FAILED; }, "the failure");
    }

    CHECK(c.auth.lastFailureWasRefusal());
    REQUIRE(c.failures.size() == 1);
    CHECK(c.failures[0].find("Tester") != std::string::npos);
    CHECK(c.successes.empty());
    CHECK(c.auth.getSessionKey().empty());
}

TEST_CASE("a login server that cannot be reached is not a refusal", "[auth][reconnect]") {
    // A port nobody listens on: the one a server had, closed again.
    uint16_t port = 0;
    {
        LoopbackLoginServer gone;
        port = gone.port();
    }
    LoopbackLoginServer unused;  // runUntil's server turns; nothing connects to it
    AuthHandler auth;
    std::vector<std::string> failures;
    auth.setOnFailure([&](const std::string& why) { failures.push_back(why); });
    auth.beginConnect("127.0.0.1", port);
    runUntil(auth, unused, [&] { return auth.getState() == AuthState::FAILED; }, "the connect to fail");
    CHECK_FALSE(auth.lastFailureWasRefusal());
    CHECK(failures.size() == 1);
}
