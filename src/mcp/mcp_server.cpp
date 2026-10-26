#include "mcp/mcp_server.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <utility>

namespace wowee {
namespace mcp {

namespace {

// Newest first: an unknown version from the client is answered with the first.
constexpr std::array<std::string_view, 3> kProtocolVersions = {
    "2025-06-18", "2025-03-26", "2024-11-05"};

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

std::string errorResponse(const nlohmann::json& id, int code, const std::string& message) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id},
                          {"error", {{"code", code}, {"message", message}}}}.dump();
}

std::string resultResponse(const nlohmann::json& id, nlohmann::json result) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump();
}

} // namespace

ToolResult ToolResult::text(std::string message) {
    ToolResult result;
    result.content.push_back({{"type", "text"}, {"text", std::move(message)}});
    return result;
}

ToolResult ToolResult::error(std::string message) {
    ToolResult result = text(std::move(message));
    result.isError = true;
    return result;
}

ToolResult ToolResult::image(std::string base64Png, std::string caption) {
    ToolResult result;
    result.content.push_back({{"type", "image"}, {"data", std::move(base64Png)}, {"mimeType", "image/png"}});
    if (!caption.empty()) result.content.push_back({{"type", "text"}, {"text", std::move(caption)}});
    return result;
}

McpServer::McpServer(std::string name, std::string version)
    : name_(std::move(name)), version_(std::move(version)) {}

void McpServer::addTool(std::string name, std::string description, nlohmann::json inputSchema,
                        ToolHandler handler) {
    tools_.push_back({std::move(name), std::move(description), std::move(inputSchema), std::move(handler)});
}

nlohmann::json McpServer::initializeResult(const nlohmann::json& params) const {
    std::string version(kProtocolVersions.front());
    if (params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
        const auto requested = params["protocolVersion"].get<std::string>();
        if (std::find(kProtocolVersions.begin(), kProtocolVersions.end(), requested) != kProtocolVersions.end())
            version = requested;
    }
    return {{"protocolVersion", version},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", name_}, {"version", version_}}}};
}

nlohmann::json McpServer::toolsList() const {
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : tools_) {
        tools.push_back({{"name", tool.name}, {"description", tool.description},
                         {"inputSchema", tool.inputSchema}});
    }
    return {{"tools", std::move(tools)}};
}

std::optional<std::string> McpServer::handle(std::string_view body) const {
    const auto request = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (request.is_discarded()) return errorResponse(nullptr, kParseError, "Parse error");

    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" ||
        !request.contains("method") || !request["method"].is_string()) {
        const auto id = request.is_object() && request.contains("id") ? request["id"] : nlohmann::json();
        return errorResponse(id, kInvalidRequest, "Invalid Request");
    }
    // A notification carries no id and is answered with nothing.
    if (!request.contains("id")) return std::nullopt;

    const auto& id = request["id"];
    const auto method = request["method"].get<std::string>();
    const auto params = request.value("params", nlohmann::json::object());

    if (method == "initialize") return resultResponse(id, initializeResult(params));
    if (method == "ping") return resultResponse(id, nlohmann::json::object());
    if (method == "tools/list") return resultResponse(id, toolsList());
    if (method == "tools/call") {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
            return errorResponse(id, kInvalidParams, "tools/call needs a tool name");
        const auto name = params["name"].get<std::string>();
        const auto tool = std::find_if(tools_.begin(), tools_.end(),
                                       [&](const Tool& t) { return t.name == name; });
        if (tool == tools_.end()) return errorResponse(id, kInvalidParams, "Unknown tool: " + name);

        const auto arguments = params.value("arguments", nlohmann::json::object());
        ToolResult result;
        try {
            result = tool->handler(arguments);
        } catch (const std::exception& e) {
            result = ToolResult::error(e.what());
        }
        return resultResponse(id, {{"content", std::move(result.content)}, {"isError", result.isError}});
    }
    return errorResponse(id, kMethodNotFound, "Method not found: " + method);
}

} // namespace mcp
} // namespace wowee
