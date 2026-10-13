#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee {

namespace ui { class UIManager; }
namespace game { class GameHandler; class ExpansionRegistry; }
namespace auth { class AuthHandler; }
namespace pipeline { class AssetManager; }

namespace core {

// Forward-declared in application.hpp
enum class AppState;

/// Handles authentication, realm selection, character selection/creation UI callbacks.
/// Owns pendingCreatedCharacterName_.
class UIScreenCallbackHandler {
public:
    using SetStateFn = std::function<void(AppState)>;

    UIScreenCallbackHandler(ui::UIManager& uiManager,
                            game::GameHandler& gameHandler,
                            auth::AuthHandler& authHandler,
                            game::ExpansionRegistry* expansionRegistry,
                            pipeline::AssetManager* assetManager,
                            SetStateFn setState);

    void setupCallbacks();

    /// Watches a world connection attempt until the character list arrives,
    /// retrying it when the server does not answer, drops it, or names a
    /// transient failure. Call every frame.
    void update(float deltaTime);
    /// What the connection is doing, for the character screen; empty when idle.
    [[nodiscard]] const std::string& connectionStatus() const { return connectionStatus_; }
    /// True while an attempt is in progress: its retries ask the login
    /// connection for the realm list, so that connection must be pumped.
    [[nodiscard]] bool connecting() const { return attempt_.active; }
    /// The realm last connected to, as the realm list gives its address;
    /// empty before the first. A realm's patch archives are mounted for it.
    [[nodiscard]] const std::string& realmAddress() const { return lastRealmAddress_; }

    /// After the world connection drops: log in again, connect to the same
    /// realm and enter the world with this character, saying each step on
    /// screen, a few times over before giving up. False when there is not
    /// enough to do it with - no realm yet, or no saved credentials.
    bool beginReconnect(uint64_t characterGuid);
    [[nodiscard]] bool reconnecting() const { return reconnect_.active; }
    void cancelReconnect();

private:
    static constexpr int kMaxAttempts = 4;
    struct ConnectAttempt {
        bool active = false;
        std::string realm;
        std::string address;
        int number = 0;          // 1-based
        int lastState = -1;      // game::WorldState, as last seen
        float phaseTime = 0.0f;  // seconds in lastState
        bool retryPending = false;
        float retryIn = 0.0f;
        std::string lastError;
    };
    void scheduleRetry(const std::string& why);
    void startReconnectRound();
    void updateReconnect(float deltaTime);

    static constexpr int kMaxReconnectRounds = 5;
    struct Reconnect {
        bool active = false;
        uint64_t characterGuid = 0;
        int round = 0;           // 1-based
        float roundTime = 0.0f;
        bool waiting = false;
        float waitIn = 0.0f;
        std::string lastError;
    };
    Reconnect reconnect_;
    // The realm last connected to, for a reconnect.
    std::string lastRealm_;
    std::string lastRealmAddress_;
    void startRetry();
    void cancelAttempt();
    static bool isRetryable(const std::string& reason);

    ui::UIManager& uiManager_;
    game::GameHandler& gameHandler_;
    auth::AuthHandler& authHandler_;
    game::ExpansionRegistry* expansionRegistry_;
    pipeline::AssetManager* assetManager_;
    SetStateFn setState_;

    std::string pendingCreatedCharacterName_;  // Auto-select after character creation

    // Connects to a realm's world server; shared by the realm screen and the
    // retries.
    std::function<void(const std::string& name, const std::string& address)> connectToRealm_;
    std::string retryRealmName_;  // waiting for a refreshed realm list
    ConnectAttempt attempt_;
    std::string connectionStatus_;
    std::vector<uint8_t> authenticatedSessionKey_;
};

} // namespace core
} // namespace wowee
