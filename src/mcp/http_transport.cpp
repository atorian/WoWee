#include "mcp/http_transport.hpp"

#include "mcp/mcp_server.hpp"
#include "network/net_platform.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>

#ifndef _WIN32
#include <poll.h>
#endif

namespace wowee {
namespace mcp {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Every byte compared whatever the first difference, so the time a refusal
// takes says nothing about how much of the token was right.
bool sameToken(std::string_view a, std::string_view b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(x ^ y);
    }
    return diff == 0;
}

const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default:  return "Error";
    }
}

HttpReply plain(int status, std::string body) { return {status, "text/plain", std::move(body)}; }

} // namespace

ParseStatus parseHttpRequest(std::string_view data, HttpRequest& out) {
    const auto headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos)
        return data.size() > kMaxHeaderBytes ? ParseStatus::TooLarge : ParseStatus::Incomplete;
    if (headerEnd > kMaxHeaderBytes) return ParseStatus::TooLarge;

    std::string_view head = data.substr(0, headerEnd);
    const auto lineEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, lineEnd);
    const auto firstSpace = requestLine.find(' ');
    const auto secondSpace = firstSpace == std::string_view::npos
        ? std::string_view::npos : requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos ||
        requestLine.substr(secondSpace + 1).rfind("HTTP/1.", 0) != 0)
        return ParseStatus::Malformed;

    HttpRequest request;
    request.method = std::string(requestLine.substr(0, firstSpace));
    request.path = std::string(requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1));

    head.remove_prefix(lineEnd == std::string_view::npos ? head.size() : lineEnd + 2);
    while (!head.empty()) {
        const auto end = head.find("\r\n");
        const auto line = head.substr(0, end);
        head.remove_prefix(end == std::string_view::npos ? head.size() : end + 2);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) return ParseStatus::Malformed;
        request.headers[lower(trim(line.substr(0, colon)))] = std::string(trim(line.substr(colon + 1)));
    }

    if (request.headers.count("transfer-encoding")) return ParseStatus::Malformed;
    size_t length = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        const auto& value = it->second;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
        if (ec != std::errc() || ptr != value.data() + value.size()) return ParseStatus::Malformed;
    }
    if (length > kMaxBodyBytes) return ParseStatus::TooLarge;
    const size_t bodyStart = headerEnd + 4;
    if (data.size() < bodyStart + length) return ParseStatus::Incomplete;

    request.body = std::string(data.substr(bodyStart, length));
    out = std::move(request);
    return ParseStatus::Complete;
}

HttpReply serveMcp(const HttpRequest& request, const McpServer& server,
                   std::string_view token, uint16_t port) {
    const auto auth = request.headers.find("authorization");
    const std::string_view bearer = "Bearer ";
    if (auth == request.headers.end() || auth->second.rfind(bearer, 0) != 0 ||
        !sameToken(std::string_view(auth->second).substr(bearer.size()), token))
        return plain(401, "Missing or wrong bearer token");

    const auto host = request.headers.find("host");
    const auto portText = ":" + std::to_string(port);
    if (request.headers.count("origin") || host == request.headers.end() ||
        (host->second != "127.0.0.1" + portText && host->second != "localhost" + portText))
        return plain(403, "Only a client on this machine may connect");

    if (request.path != "/mcp") return plain(404, "Not found");
    if (request.method != "POST") return plain(405, "POST only");

    const auto response = server.handle(request.body);
    if (!response) return {202, {}, {}};
    return {200, "application/json", *response};
}

std::string formatHttpReply(const HttpReply& reply) {
    std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " " + reasonPhrase(reply.status) + "\r\n";
    if (!reply.contentType.empty()) out += "Content-Type: " + reply.contentType + "\r\n";
    if (reply.status == 405) out += "Allow: POST\r\n";
    out += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += reply.body;
    return out;
}

HttpListener::~HttpListener() { stop(); }

bool HttpListener::start(uint16_t port, Handler handler, std::string& error) {
    stop();
    net::ensureInit();
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) { error = "cannot create a socket"; return false; }
    int reuse = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 4) != 0) {
        error = "cannot listen on 127.0.0.1:" + std::to_string(port);
        net::closeSocket(s);
        return false;
    }
    socklen_t length = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &length);

    handler_ = std::move(handler);
    listenSocket_ = static_cast<intptr_t>(s);
    port_ = ntohs(addr.sin_port);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void HttpListener::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    net::closeSocket(static_cast<socket_t>(listenSocket_));
    listenSocket_ = -1;
}

void HttpListener::run() {
    const auto s = static_cast<socket_t>(listenSocket_);
    while (running_) {
        // Woken every 200 ms to see whether stop() was called.
#ifdef _WIN32
        WSAPOLLFD item{s, POLLRDNORM, 0};
        const int ready = WSAPoll(&item, 1, 200);
#else
        pollfd item{s, POLLIN, 0};
        const int ready = ::poll(&item, 1, 200);
#endif
        if (ready <= 0) continue;
        const socket_t client = ::accept(s, nullptr, nullptr);
        if (client == INVALID_SOCK) continue;
        serve(static_cast<intptr_t>(client));
        net::closeSocket(client);
    }
}

void HttpListener::serve(intptr_t clientHandle) {
    const auto client = static_cast<socket_t>(clientHandle);
    std::string data;
    HttpRequest request;
    ParseStatus status = ParseStatus::Incomplete;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    char buffer[8192];
    while (status == ParseStatus::Incomplete && std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
        WSAPOLLFD item{client, POLLRDNORM, 0};
        if (WSAPoll(&item, 1, 200) <= 0) continue;
#else
        pollfd item{client, POLLIN, 0};
        if (::poll(&item, 1, 200) <= 0) continue;
#endif
        const auto n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        data.append(buffer, static_cast<size_t>(n));
        status = parseHttpRequest(data, request);
    }

    HttpReply reply;
    if (status == ParseStatus::Complete) reply = handler_(request);
    else if (status == ParseStatus::TooLarge) reply = plain(413, "Request too large");
    else if (status == ParseStatus::Malformed) reply = plain(400, "Malformed request");
    else return;

    const auto text = formatHttpReply(reply);
    size_t sent = 0;
    while (sent < text.size()) {
        const auto n = ::send(client, text.data() + sent, static_cast<int>(text.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

} // namespace mcp
} // namespace wowee
