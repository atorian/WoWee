// The MCP endpoint over HTTP: parsing, who is let in, and a real round trip.
//
// The rules are the transport's: Content-Length bodies, a bearer token, no
// browser pages (an Origin header) and no other Host - the DNS-rebinding
// guard the MCP specification asks local servers for - POST only, 202 for a
// notification.
#include <catch_amalgamated.hpp>

#include "mcp/http_transport.hpp"
#include "mcp/mcp_server.hpp"
#include "network/net_platform.hpp"

#include <string>

using namespace wowee::mcp;
using nlohmann::json;

namespace {

HttpRequest post(std::string body, std::string token = "secret", uint16_t port = 8765) {
    HttpRequest r;
    r.method = "POST";
    r.path = "/mcp";
    r.headers["host"] = "127.0.0.1:" + std::to_string(port);
    if (!token.empty()) r.headers["authorization"] = "Bearer " + token;
    r.body = std::move(body);
    return r;
}

McpServer pingServer() { return McpServer("wowee", "1"); }

const std::string kPing = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";

} // namespace

TEST_CASE("an HTTP request is parsed once its body has arrived", "[mcp][http]") {
    HttpRequest r;

    SECTION("complete, header names lower-cased") {
        const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:1\r\nContent-Length: 4\r\n"
                                "Authorization: Bearer x\r\n\r\nabcd";
        REQUIRE(parseHttpRequest(raw, r) == ParseStatus::Complete);
        CHECK(r.method == "POST");
        CHECK(r.path == "/mcp");
        CHECK(r.headers.at("authorization") == "Bearer x");
        CHECK(r.body == "abcd");
    }
    SECTION("headers not finished") {
        CHECK(parseHttpRequest("POST /mcp HTTP/1.1\r\nHost: a", r) == ParseStatus::Incomplete);
    }
    SECTION("body shorter than its Content-Length") {
        CHECK(parseHttpRequest("POST /mcp HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", r) ==
              ParseStatus::Incomplete);
    }
    SECTION("no request line") {
        CHECK(parseHttpRequest("garbage\r\n\r\n", r) == ParseStatus::Malformed);
    }
    SECTION("a chunked body is refused") {
        CHECK(parseHttpRequest("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", r) ==
              ParseStatus::Malformed);
    }
    SECTION("a body over the limit is refused before it is read") {
        CHECK(parseHttpRequest("POST /mcp HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n", r) ==
              ParseStatus::TooLarge);
    }
}

TEST_CASE("only this machine's client with the token reaches the server", "[mcp][http]") {
    const auto server = pingServer();

    SECTION("the right token gets the JSON-RPC response") {
        const auto reply = serveMcp(post(kPing), server, "secret", 8765);
        CHECK(reply.status == 200);
        CHECK(reply.contentType == "application/json");
        CHECK(json::parse(reply.body).at("result") == json::object());
    }
    SECTION("no token") {
        CHECK(serveMcp(post(kPing, ""), server, "secret", 8765).status == 401);
    }
    SECTION("a wrong token, even one that starts the same") {
        CHECK(serveMcp(post(kPing, "secre"), server, "secret", 8765).status == 401);
        CHECK(serveMcp(post(kPing, "secretX"), server, "secret", 8765).status == 401);
    }
    SECTION("a browser page") {
        auto r = post(kPing);
        r.headers["origin"] = "https://example.com";
        CHECK(serveMcp(r, server, "secret", 8765).status == 403);
    }
    SECTION("another Host, as DNS rebinding sends") {
        auto r = post(kPing);
        r.headers["host"] = "evil.example:8765";
        CHECK(serveMcp(r, server, "secret", 8765).status == 403);
    }
    SECTION("localhost is the same machine") {
        auto r = post(kPing);
        r.headers["host"] = "localhost:8765";
        CHECK(serveMcp(r, server, "secret", 8765).status == 200);
    }
    SECTION("another path") {
        auto r = post(kPing);
        r.path = "/other";
        CHECK(serveMcp(r, server, "secret", 8765).status == 404);
    }
    SECTION("GET, which would open an event stream this server does not offer") {
        auto r = post("");
        r.method = "GET";
        CHECK(serveMcp(r, server, "secret", 8765).status == 405);
    }
    SECTION("a notification is accepted with no body") {
        const auto reply = serveMcp(post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"),
                                    server, "secret", 8765);
        CHECK(reply.status == 202);
        CHECK(reply.body.empty());
    }
}

TEST_CASE("a reply is a complete HTTP/1.1 response", "[mcp][http]") {
    const auto text = formatHttpReply({200, "application/json", "{}"});
    CHECK(text.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(text.find("Content-Type: application/json\r\n") != std::string::npos);
    CHECK(text.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(text.find("Connection: close\r\n") != std::string::npos);
    CHECK(text.substr(text.size() - 6) == "\r\n\r\n{}");
}

TEST_CASE("the listener answers a request over a real socket", "[mcp][http]") {
    wowee::net::ensureInit();
    const auto server = pingServer();
    HttpListener listener;
    std::string error;
    REQUIRE(listener.start(0, [&](const HttpRequest& r) {
                return serveMcp(r, server, "secret", listener.port());
            }, error));
    REQUIRE(listener.port() != 0);

    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(s != INVALID_SOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listener.port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    const std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(listener.port()) +
                                "\r\nAuthorization: Bearer secret\r\nContent-Type: application/json\r\n"
                                "Content-Length: " + std::to_string(kPing.size()) + "\r\n\r\n" + kPing;
    REQUIRE(::send(s, request.data(), static_cast<int>(request.size()), 0) == static_cast<ssize_t>(request.size()));

    std::string response;
    char buffer[4096];
    for (;;) {
        const auto n = ::recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<size_t>(n));
    }
    wowee::net::closeSocket(s);
    listener.stop();

    REQUIRE(response.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    const auto body = response.substr(response.find("\r\n\r\n") + 4);
    CHECK(json::parse(body).at("id") == 1);
}
