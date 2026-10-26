#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>

namespace wowee {
namespace mcp {

class McpServer;

/// One HTTP/1.1 request. Header names are lower-cased.
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpReply {
    int status = 200;
    std::string contentType;
    std::string body;
};

enum class ParseStatus { Incomplete, Complete, Malformed, TooLarge };

inline constexpr size_t kMaxHeaderBytes = 16 * 1024;
inline constexpr size_t kMaxBodyBytes = 1024 * 1024;

/// Parses one request from the bytes read so far. A body needs Content-Length;
/// chunked bodies are refused as malformed.
ParseStatus parseHttpRequest(std::string_view data, HttpRequest& out);

/// The streamable-HTTP MCP endpoint at /mcp, for a client on this machine.
///
/// Refused before the server sees the body: a request without the bearer
/// token (401), one from a browser page - it carries an Origin - or naming
/// another Host, which is how DNS rebinding reaches a local port (403),
/// another path (404), anything but POST (405). A POST answers 200 with the
/// JSON-RPC response, or 202 with no body for a notification.
HttpReply serveMcp(const HttpRequest& request, const McpServer& server,
                   std::string_view token, uint16_t port);

std::string formatHttpReply(const HttpReply& reply);

/// Accepts connections on 127.0.0.1, one at a time, on its own thread, and
/// answers each with one reply before closing it.
class HttpListener {
public:
    using Handler = std::function<HttpReply(const HttpRequest&)>;

    HttpListener() = default;
    ~HttpListener();
    HttpListener(const HttpListener&) = delete;
    HttpListener& operator=(const HttpListener&) = delete;

    /// Port 0 picks a free one; port() says which. False, with the reason in
    /// error, when the port cannot be bound.
    bool start(uint16_t port, Handler handler, std::string& error);
    void stop();
    [[nodiscard]] uint16_t port() const { return port_; }

private:
    void run();
    void serve(intptr_t client);

    Handler handler_;
    intptr_t listenSocket_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace mcp
} // namespace wowee
