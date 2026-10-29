#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace mcp {

/// What a tool call answers: MCP content blocks, and whether it failed.
///
/// A refused or failed call is still a result, not a protocol error: the
/// agent reads the text and decides what to do, as it would with any other
/// tool.
struct ToolResult {
    nlohmann::json content = nlohmann::json::array();
    bool isError = false;

    static ToolResult text(std::string message);
    static ToolResult error(std::string message);
    /// A PNG, already base64-encoded, with an optional caption after it.
    static ToolResult image(std::string base64Png, std::string caption = {});
};

using ToolHandler = std::function<ToolResult(const nlohmann::json& arguments)>;

/// The Model Context Protocol over JSON-RPC 2.0, without a transport.
///
/// Answers initialize, ping, tools/list and tools/call for the tools added to
/// it. handle() takes one request body and returns the response body, or
/// nothing for a notification, which gets no response.
class McpServer {
public:
    McpServer(std::string name, std::string version);

    /// inputSchema is the JSON Schema of the arguments object.
    void addTool(std::string name, std::string description, nlohmann::json inputSchema,
                 ToolHandler handler);

    [[nodiscard]] std::optional<std::string> handle(std::string_view body) const;

private:
    struct Tool {
        std::string name;
        std::string description;
        nlohmann::json inputSchema;
        ToolHandler handler;
    };

    nlohmann::json initializeResult(const nlohmann::json& params) const;
    nlohmann::json toolsList() const;

    std::string name_;
    std::string version_;
    std::vector<Tool> tools_;
};

} // namespace mcp
} // namespace wowee
