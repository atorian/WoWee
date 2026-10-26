// MCP over JSON-RPC 2.0, the transport left out.
//
// What the client expects is the specification's, not this code's: the
// lifecycle (initialize, notifications get no answer), tools/list and
// tools/call, and the JSON-RPC error codes -32700, -32600, -32601, -32602.
// A tool that fails reports it inside its result (isError), which is how the
// specification separates a tool error from a protocol error.
#include <catch_amalgamated.hpp>

#include "mcp/mcp_server.hpp"

#include <stdexcept>

using wowee::mcp::McpServer;
using wowee::mcp::ToolResult;
using nlohmann::json;

namespace {

McpServer serverWithEcho() {
    McpServer server("wowee", "1.2.3");
    server.addTool("echo", "Repeat the text back",
                   json{{"type", "object"},
                        {"properties", {{"text", {{"type", "string"}}}}},
                        {"required", {"text"}}},
                   [](const json& args) { return ToolResult::text(args.at("text").get<std::string>()); });
    server.addTool("broken", "Always throws", json{{"type", "object"}},
                   [](const json&) -> ToolResult { throw std::runtime_error("boom"); });
    return server;
}

json call(const McpServer& server, const json& request) {
    const auto response = server.handle(request.dump());
    REQUIRE(response.has_value());
    return json::parse(*response);
}

} // namespace

TEST_CASE("initialize answers with the protocol version, server and tool capability",
          "[mcp]") {
    const auto server = serverWithEcho();

    SECTION("a version the server speaks is echoed") {
        const auto r = call(server, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                     {"params", {{"protocolVersion", "2025-03-26"},
                                                 {"capabilities", json::object()},
                                                 {"clientInfo", {{"name", "t"}, {"version", "0"}}}}}});
        CHECK(r.at("jsonrpc") == "2.0");
        CHECK(r.at("id") == 1);
        CHECK(r.at("result").at("protocolVersion") == "2025-03-26");
        CHECK(r.at("result").at("serverInfo").at("name") == "wowee");
        CHECK(r.at("result").at("serverInfo").at("version") == "1.2.3");
        CHECK(r.at("result").at("capabilities").contains("tools"));
    }
    SECTION("an unknown version gets the newest one the server speaks") {
        const auto r = call(server, {{"jsonrpc", "2.0"}, {"id", "a"}, {"method", "initialize"},
                                     {"params", {{"protocolVersion", "1999-01-01"}}}});
        CHECK(r.at("id") == "a");
        CHECK(r.at("result").at("protocolVersion") == "2025-06-18");
    }
}

TEST_CASE("a notification gets no response", "[mcp]") {
    const auto server = serverWithEcho();
    CHECK_FALSE(server.handle(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump())
                    .has_value());
}

TEST_CASE("tools/list names each tool with its description and schema", "[mcp]") {
    const auto r = call(serverWithEcho(), {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    const auto& tools = r.at("result").at("tools");
    REQUIRE(tools.size() == 2);
    CHECK(tools[0].at("name") == "echo");
    CHECK(tools[0].at("description") == "Repeat the text back");
    CHECK(tools[0].at("inputSchema").at("required")[0] == "text");
    CHECK(tools[1].at("name") == "broken");
}

TEST_CASE("tools/call runs the tool with its arguments", "[mcp]") {
    const auto server = serverWithEcho();

    SECTION("a result comes back as content") {
        const auto r = call(server, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                                     {"params", {{"name", "echo"}, {"arguments", {{"text", "hi"}}}}}});
        const auto& result = r.at("result");
        CHECK(result.at("isError") == false);
        CHECK(result.at("content")[0].at("type") == "text");
        CHECK(result.at("content")[0].at("text") == "hi");
    }
    SECTION("a tool that throws reports it as a tool error, not a protocol error") {
        const auto r = call(server, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
                                     {"params", {{"name", "broken"}, {"arguments", json::object()}}}});
        CHECK_FALSE(r.contains("error"));
        CHECK(r.at("result").at("isError") == true);
        CHECK(r.at("result").at("content")[0].at("text") == "boom");
    }
    SECTION("an unknown tool is invalid params") {
        const auto r = call(server, {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                                     {"params", {{"name", "nope"}}}});
        CHECK(r.at("error").at("code") == -32602);
    }
}

TEST_CASE("ping answers with an empty result", "[mcp]") {
    const auto r = call(serverWithEcho(), {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "ping"}});
    CHECK(r.at("result") == json::object());
}

TEST_CASE("broken requests get the JSON-RPC error codes", "[mcp]") {
    const auto server = serverWithEcho();

    SECTION("not JSON: parse error with a null id") {
        const auto response = server.handle("{not json");
        REQUIRE(response.has_value());
        const auto r = json::parse(*response);
        CHECK(r.at("error").at("code") == -32700);
        CHECK(r.at("id").is_null());
    }
    SECTION("not a JSON-RPC 2.0 request: invalid request") {
        const auto r = call(server, {{"id", 7}, {"method", "ping"}});
        CHECK(r.at("error").at("code") == -32600);
    }
    SECTION("an unknown method") {
        const auto r = call(server, {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "resources/list"}});
        CHECK(r.at("error").at("code") == -32601);
        CHECK(r.at("id") == 8);
    }
}

TEST_CASE("an image result carries the PNG and its caption", "[mcp]") {
    const auto result = ToolResult::image("iVBORw0KGgo=", "Dalaran");
    REQUIRE(result.content.size() == 2);
    CHECK(result.content[0].at("type") == "image");
    CHECK(result.content[0].at("mimeType") == "image/png");
    CHECK(result.content[0].at("data") == "iVBORw0KGgo=");
    CHECK(result.content[1].at("text") == "Dalaran");
}
