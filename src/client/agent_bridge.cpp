#include "client/agent_bridge.hpp"
#include "audio/audio_coordinator.hpp"

#include "platform/config_paths.hpp"
#include "foundation/logger.hpp"
#include "game/game_handler.hpp"
#include "mcp/http_transport.hpp"
#include "mcp/main_thread_queue.hpp"
#include "mcp/mcp_server.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"
#include "ui/agent_controls.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace wowee {
namespace core {

namespace {

using nlohmann::json;
using mcp::ToolResult;
using namespace std::chrono_literals;

constexpr auto kToolTimeout = 3s;

/// The token in <config root>/mcp_token, made on first use, this user's alone.
bool loadOrCreateToken(std::string& token, std::string& error) {
    const std::filesystem::path path = std::filesystem::path(getConfigRoot()) / "mcp_token";
    if (std::ifstream in(path); in) {
        std::getline(in, token);
        if (token.size() >= 32) return true;
    }
    unsigned char bytes[24];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) { error = "cannot generate a token"; return false; }
    static constexpr char hex[] = "0123456789abcdef";
    token.clear();
    for (unsigned char b : bytes) { token += hex[b >> 4]; token += hex[b & 15]; }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) { error = "cannot write " + path.string(); return false; }
    out << token << '\n';
    out.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return true;
}

std::string base64(const std::vector<unsigned char>& bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), bytes.data(),
                                  static_cast<int>(bytes.size()));
    out.resize(static_cast<size_t>(n));
    return out;
}

json numberProperty(const char* description) {
    return {{"type", "number"}, {"description", description}};
}

json objectSchema(json properties = json::object(), json required = json::array()) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}

constexpr const char* kNoControl =
    "The player has not handed control over. Ask them to press Ctrl+Shift+G in the game, or type /ai on in its chat.";

} // namespace

struct AgentBridge::Impl {
    Dependencies deps;
    mcp::McpServer server{"wowee", "1"};
    mcp::MainThreadQueue queue;
    mcp::HttpListener listener;
    std::string token;
    float fps = 0.0f;

    explicit Impl(Dependencies d) : deps(d) { addTools(); }

    /// Every tool touches the game, so every tool runs on the frame loop.
    void addTool(const char* name, const char* description, json schema,
                 std::function<ToolResult(const json&)> body) {
        server.addTool(name, description, std::move(schema),
                       [this, body = std::move(body)](const json& args) {
                           return queue.run([&] { return body(args); }, kToolTimeout);
                       });
    }

    rendering::CameraController* camera() const {
        return deps.camera;
    }

    void addTools() {
        addTool("get_state", "Where the character is and what the view shows: zone, position, "
                "facing, camera angles, frame rate, and who has control.", objectSchema(),
                [this](const json&) {
                    json state = {{"inWorld", deps.gameHandler && deps.gameHandler->isInWorld()},
                                  {"control", deps.controls && deps.controls->hasControl() ? "agent" : "player"},
                                  {"fps", std::round(fps * 10.0f) / 10.0f}};
                    if (deps.renderer) {
                        const auto& p = deps.renderer->getCharacterPosition();
                        if (deps.audio) state["zone"] = deps.audio->getCurrentZoneName();
                        state["position"] = {{"x", p.x}, {"y", p.y}, {"z", p.z}};
                        state["facingDegrees"] = deps.renderer->getCharacterYaw();
                    }
                    if (auto* cam = camera()) state["camera"] = {{"yaw", cam->getYaw()}, {"pitch", cam->getPitch()}};
                    return ToolResult::text(state.dump());
                });

        addTool("move", "Hold a movement key for some seconds: forward, backward, strafe_left or "
                "strafe_right. Needs control (Ctrl+Shift+G or /ai on).",
                objectSchema({{"direction", {{"type", "string"},
                                             {"enum", {"forward", "backward", "strafe_left", "strafe_right"}}}},
                              {"seconds", numberProperty("How long to hold it, 0.05 to 10")}},
                             {"direction", "seconds"}),
                [this](const json& args) {
                    using Move = ui::AgentControls::Move;
                    const auto direction = args.at("direction").get<std::string>();
                    const double seconds = args.at("seconds").get<double>();
                    if (seconds < 0.05 || seconds > 10.0) return ToolResult::error("seconds must be 0.05 to 10");
                    const Move move = direction == "forward" ? Move::Forward
                                    : direction == "backward" ? Move::Backward
                                    : direction == "strafe_left" ? Move::StrafeLeft : Move::StrafeRight;
                    if (!deps.controls->move(move, static_cast<float>(seconds))) return ToolResult::error(kNoControl);
                    return ToolResult::text("Moving " + direction + " for " + std::to_string(seconds) + " s");
                });

        addTool("turn", "Turn the character and the view together. Positive degrees turn right. "
                "Needs control (Ctrl+Shift+G or /ai on).",
                objectSchema({{"degrees", numberProperty("-360 to 360")}}, {"degrees"}),
                [this](const json& args) {
                    const double degrees = args.at("degrees").get<double>();
                    if (std::abs(degrees) > 360.0) return ToolResult::error("degrees must be -360 to 360");
                    if (!deps.controls->turn(static_cast<float>(degrees))) return ToolResult::error(kNoControl);
                    return ToolResult::text("Turned " + std::to_string(degrees) + " degrees");
                });

        addTool("look", "Move the view without turning the character: yaw (positive right) and "
                "pitch (positive up), in degrees. Needs control (Ctrl+Shift+G or /ai on).",
                objectSchema({{"yaw", numberProperty("degrees")}, {"pitch", numberProperty("degrees")}}),
                [this](const json& args) {
                    const double yaw = args.value("yaw", 0.0);
                    const double pitch = args.value("pitch", 0.0);
                    if (!deps.controls->look(static_cast<float>(yaw), static_cast<float>(pitch)))
                        return ToolResult::error(kNoControl);
                    return ToolResult::text("Looked");
                });

        addTool("zoom", "Move the camera in (positive) or out (negative) by mouse-wheel notches. "
                "Needs control (Ctrl+Shift+G or /ai on).",
                objectSchema({{"notches", numberProperty("-20 to 20")}}, {"notches"}),
                [this](const json& args) {
                    const double notches = args.at("notches").get<double>();
                    if (std::abs(notches) > 20.0) return ToolResult::error("notches must be -20 to 20");
                    if (!deps.controls->zoom(static_cast<float>(notches))) return ToolResult::error(kNoControl);
                    return ToolResult::text("Zoomed");
                });

        addTool("jump", "Jump once. Needs control (Ctrl+Shift+G or /ai on).", objectSchema(),
                [this](const json&) {
                    if (!deps.controls->jump()) return ToolResult::error(kNoControl);
                    return ToolResult::text("Jumped");
                });

        addTool("stop", "Let go of every movement key.", objectSchema(),
                [this](const json&) {
                    deps.controls->stop();
                    return ToolResult::text("Stopped");
                });

        addTool("release_control", "Give control back to the player.", objectSchema(),
                [this](const json&) {
                    deps.controls->release();
                    return ToolResult::text("The player has control");
                });

        addTool("screenshot", "What is on the screen now, as a PNG.", objectSchema(),
                [this](const json&) {
                    if (!deps.renderer) return ToolResult::error("No renderer");
                    const auto path = std::filesystem::temp_directory_path() / "wowee-mcp-screenshot.png";
                    if (!deps.renderer->captureScreenshot(path.string()))
                        return ToolResult::error("The screenshot could not be taken");
                    std::ifstream in(path, std::ios::binary);
                    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                                           std::istreambuf_iterator<char>());
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                    return ToolResult::image(base64(bytes));
                });
    }
};

AgentBridge::AgentBridge(Dependencies dependencies) : impl_(std::make_unique<Impl>(dependencies)) {}
AgentBridge::~AgentBridge() { impl_->listener.stop(); }

bool AgentBridge::start(uint16_t port, std::string& error) {
    if (!loadOrCreateToken(impl_->token, error)) return false;
    auto* impl = impl_.get();
    if (!impl_->listener.start(port, [impl](const mcp::HttpRequest& request) {
            return mcp::serveMcp(request, impl->server, impl->token, impl->listener.port());
        }, error))
        return false;
    LOG_WARNING("MCP endpoint at http://127.0.0.1:", impl_->listener.port(), "/mcp, bearer token in ",
                (std::filesystem::path(getConfigRoot()) / "mcp_token").string());
    return true;
}

void AgentBridge::pump(float deltaTime) {
    if (deltaTime > 0.0f) {
        const float instant = 1.0f / deltaTime;
        impl_->fps = impl_->fps == 0.0f ? instant : impl_->fps * 0.95f + instant * 0.05f;
    }
    impl_->queue.drain();
}

} // namespace core
} // namespace wowee
