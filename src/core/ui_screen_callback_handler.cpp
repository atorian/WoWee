#include "core/ui_screen_callback_handler.hpp"
#include "core/application.hpp"  // AppState
#include "core/logger.hpp"
#include "ui/ui_manager.hpp"
#include "auth/auth_handler.hpp"
#include "game/game_handler.hpp"
#include "game/expansion_profile.hpp"
#include "game/world_packet_data.hpp"
#include "pipeline/asset_manager.hpp"
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <utility>

namespace wowee { namespace core {

UIScreenCallbackHandler::UIScreenCallbackHandler(
    ui::UIManager& uiManager,
    game::GameHandler& gameHandler,
    auth::AuthHandler& authHandler,
    game::ExpansionRegistry* expansionRegistry,
    pipeline::AssetManager* assetManager,
    SetStateFn setState)
    : uiManager_(uiManager)
    , gameHandler_(gameHandler)
    , authHandler_(authHandler)
    , expansionRegistry_(expansionRegistry)
    , assetManager_(assetManager)
    , setState_(std::move(setState))
{
}

void UIScreenCallbackHandler::setupCallbacks() {
    gameHandler_.setOnFailure([this](const std::string& reason) {
        // While connecting, a transient failure is retried rather than shown:
        // Isengard answers a first attempt with REALM_LIST_REALM_NOT_FOUND, or
        // with nothing at all, and admits the next one.
        if (attempt_.active && isRetryable(reason)) {
            scheduleRetry(reason);
            return;
        }
        cancelAttempt();
        uiManager_.getAuthScreen().setStatus(reason, true, true);
        setState_(AppState::AUTHENTICATION);
    });
    authHandler_.setOnSuccess([this](const std::vector<uint8_t>& sessionKey) {
        authenticatedSessionKey_ = sessionKey;
        LOG_INFO("Cached auth session key for world handoff (", authenticatedSessionKey_.size(), " bytes)");
    });

    // Authentication screen callback
    uiManager_.getAuthScreen().setOnSuccess([this]() {
        LOG_INFO("Authentication successful, transitioning to realm selection");
        setState_(AppState::REALM_SELECTION);
        if (reconnect_.active) {
            // Straight back to the realm that was lost, through the same
            // watched attempt the realm screen starts: the realm list is asked
            // for again and the realm connected to as soon as it is in.
            attempt_ = {};
            attempt_.active = true;
            attempt_.number = 1;
            attempt_.realm = lastRealm_;
            attempt_.address = lastRealmAddress_;
            retryRealmName_ = lastRealm_;
            uiManager_.getCharacterScreen().enterWorldWhenListed(reconnect_.characterGuid);
        }
    });

    authHandler_.setOnRealmList([this](const std::vector<auth::Realm>& realms) {
        if (retryRealmName_.empty()) return;
        const std::string name = std::exchange(retryRealmName_, std::string());
        for (const auto& realm : realms) {
            if (realm.name != name) continue;
            LOG_INFO("Realm list refreshed: '", name, "' is at ", realm.address);
            connectToRealm_(realm.name, realm.address);
            return;
        }
        LOG_WARNING("Realm '", name, "' is no longer in the realm list; using its last address");
        if (attempt_.active) connectToRealm_(name, attempt_.address);
    });

    // Realm selection callback
    uiManager_.getRealmScreen().setOnRealmSelected([this](const std::string& realmName, const std::string& realmAddress) {
        cancelReconnect();
        retryRealmName_.clear();
        attempt_ = {};
        attempt_.number = 1;
        connectToRealm_(realmName, realmAddress);
    });

    connectToRealm_ = [this](const std::string& realmName, const std::string& realmAddress) {
        LOG_INFO("Realm selected: ", realmName, " (", realmAddress, ")");
        lastRealm_ = realmName;
        lastRealmAddress_ = realmAddress;
        attempt_.active = true;
        attempt_.realm = realmName;
        attempt_.address = realmAddress;
        attempt_.lastState = -1;
        attempt_.phaseTime = 0.0f;

        // Parse realm address (format: "hostname:port")
        std::string host = realmAddress;
        uint16_t port = 8085;  // Default world server port

        size_t colonPos = realmAddress.find(':');
        if (colonPos != std::string::npos) {
            host = realmAddress.substr(0, colonPos);
            try { port = static_cast<uint16_t>(std::stoi(realmAddress.substr(colonPos + 1))); }
            catch (...) { LOG_WARNING("Invalid port in realm address: ", realmAddress); }
        }

        // LAN clients are often handed a public realm address by MaNGOS. On
        // routers without NAT reflection that address cannot hairpin back to
        // the local world server. Allow a client-side host override while
        // retaining the realm-advertised port and authentication identity.
        if (const char* overrideHost = std::getenv("WOWEE_REALM_HOST_OVERRIDE");
            overrideHost && *overrideHost) {
            LOG_WARNING("Overriding realm host '", host, "' with '", overrideHost,
                        "' via WOWEE_REALM_HOST_OVERRIDE");
            host = overrideHost;
        }

        // Connect to world server
        auto sessionKey = authHandler_.getSessionKey();
        if (sessionKey.empty() && !authenticatedSessionKey_.empty()) {
            LOG_WARNING("Auth handler session key was empty at realm selection; using cached key");
            sessionKey = authenticatedSessionKey_;
        }
        if (sessionKey.size() != 40) {
            LOG_ERROR("Cannot connect to realm: auth session key has ", sessionKey.size(),
                      " bytes; expected 40. Re-authenticate and try again.");
        }
        std::string accountName = authHandler_.getUsername();
        if (accountName.empty()) {
            LOG_WARNING("Auth username missing; falling back to TESTACCOUNT");
            accountName = "TESTACCOUNT";
        }

        uint32_t realmId = 0;
        uint16_t realmBuild = 0;
        {
            // WotLK AUTH_SESSION includes a RealmID field; some servers reject if it's wrong/zero.
            const auto& realms = authHandler_.getRealms();
            for (const auto& r : realms) {
                if (r.name == realmName && r.address == realmAddress) {
                    realmId = r.id;
                    realmBuild = r.build;
                    break;
                }
            }
            LOG_INFO("Selected realmId=", realmId, " realmBuild=", realmBuild);
        }

        uint32_t clientBuild = 12340; // default WotLK
        if (expansionRegistry_) {
            auto* profile = expansionRegistry_->getActive();
            if (profile) clientBuild = profile->worldBuild;
        }
        // Prefer realm-reported build when available (e.g. vanilla servers
        // that report build 5875 in the realm list)
        if (realmBuild != 0) {
            clientBuild = realmBuild;
            LOG_INFO("Using realm-reported build: ", clientBuild);
        }
        if (gameHandler_.connect(host, port, sessionKey, accountName, clientBuild, realmId,
                authHandler_.getServerHost(), authHandler_.getServerPort())) {
            LOG_INFO("Connected to world server, transitioning to character selection");
            setState_(AppState::CHARACTER_SELECTION);
        } else {
            LOG_ERROR("Failed to connect to world server");
            if (attempt_.active) {
                scheduleRetry("could not reach the world server at " + realmAddress);
                return;
            }
            setState_(AppState::AUTHENTICATION);
        }
    };

    // Realm screen back button - return to login
    uiManager_.getRealmScreen().setOnBack([this]() {
        cancelReconnect();
        cancelAttempt();
        authHandler_.disconnect();
        uiManager_.getRealmScreen().reset();
        setState_(AppState::AUTHENTICATION);
    });

    // Character selection callback
    uiManager_.getCharacterScreen().setOnCharacterSelected([this](uint64_t characterGuid) {
        LOG_INFO("Character selected: GUID=0x", std::hex, characterGuid, std::dec);
        // Always set the active character GUID
        gameHandler_.setActiveCharacterGuid(characterGuid);
        // Keep CHARACTER_SELECTION active until world entry is fully loaded.
        // This avoids exposing pre-load hitching before the loading screen/intro.
    });

    // Character create screen callbacks
    uiManager_.getCharacterCreateScreen().setOnCreate([this](const game::CharCreateData& data) {
        pendingCreatedCharacterName_ = data.name;  // Store name for auto-selection
        gameHandler_.createCharacter(data);
    });

    uiManager_.getCharacterCreateScreen().setOnCancel([this]() {
        setState_(AppState::CHARACTER_SELECTION);
    });

    // Character create result callback
    gameHandler_.setCharCreateCallback([this](bool success, const std::string& msg) {
        if (success) {
            // Auto-select the newly created character
            if (!pendingCreatedCharacterName_.empty()) {
                uiManager_.getCharacterScreen().selectCharacterByName(pendingCreatedCharacterName_);
                pendingCreatedCharacterName_.clear();
            }
            setState_(AppState::CHARACTER_SELECTION);
        } else {
            uiManager_.getCharacterCreateScreen().setStatus(msg, true);
            pendingCreatedCharacterName_.clear();
        }
    });

    // Character login failure callback
    gameHandler_.setCharLoginFailCallback([this](const std::string& reason) {
        LOG_WARNING("Character login failed: ", reason);
        setState_(AppState::CHARACTER_SELECTION);
        uiManager_.getCharacterScreen().setStatus("Login failed: " + reason, true);
    });

    // "New Hero" button on character screen
    uiManager_.getCharacterScreen().setOnCreateCharacter([this]() {
        uiManager_.getCharacterCreateScreen().reset();
        // Apply expansion race/class constraints before showing the screen
        if (expansionRegistry_ && expansionRegistry_->getActive()) {
            auto* profile = expansionRegistry_->getActive();
            uiManager_.getCharacterCreateScreen().setExpansionConstraints(
                profile->races, profile->classes);
        }
        uiManager_.getCharacterCreateScreen().initializePreview(assetManager_);
        setState_(AppState::CHARACTER_CREATION);
    });

    // "Back" button on character screen
    uiManager_.getCharacterScreen().setOnBack([this]() {
        // Disconnect from world server and reset UI state for fresh realm selection
        cancelReconnect();
        cancelAttempt();
        gameHandler_.disconnect();
        uiManager_.getRealmScreen().resetForBack();
        uiManager_.getCharacterScreen().reset();
        setState_(AppState::REALM_SELECTION);
    });

    // "Delete Character" button on character screen
    uiManager_.getCharacterScreen().setOnDeleteCharacter([this](uint64_t guid) {
        gameHandler_.deleteCharacter(guid);
    });

    // Character delete result callback
    gameHandler_.setCharDeleteCallback([this](bool success, const std::string& message) {
        uiManager_.getCharacterScreen().setStatus(message, !success);
        if (success) {
            gameHandler_.requestCharacterList();
        }
    });
}

bool UIScreenCallbackHandler::isRetryable(const std::string& reason) {
    // Answers about the account or the client: another attempt gets the same
    // one, and some of them count failed logins.
    static const char* const kFinal[] = {
        "BANNED", "SUSPENDED", "UNKNOWN_ACCOUNT", "INCORRECT_PASSWORD", "VERSION_MISMATCH",
        "PARENTAL_CONTROL", "LOCKED_ENFORCED", "BILLING", "NO_TIME", "EZWoW",
    };
    for (const char* word : kFinal) {
        if (reason.find(word) != std::string::npos) return false;
    }
    return true;
}

void UIScreenCallbackHandler::cancelAttempt() {
    attempt_ = {};
    retryRealmName_.clear();
    connectionStatus_.clear();
}

void UIScreenCallbackHandler::scheduleRetry(const std::string& why) {
    if (!attempt_.active || attempt_.retryPending) return;
    gameHandler_.disconnect();
    attempt_.lastError = why;
    if (attempt_.number >= kMaxAttempts) {
        const std::string message = "Could not connect to " + attempt_.realm + " after " +
                                    std::to_string(kMaxAttempts) + " attempts: " + why;
        LOG_ERROR(message);
        cancelAttempt();
        uiManager_.getAuthScreen().setStatus(message, true, true);
        setState_(AppState::AUTHENTICATION);
        return;
    }
    // A little longer each time, so a server that is restarting gets room.
    attempt_.retryPending = true;
    attempt_.retryIn = static_cast<float>(attempt_.number);
    LOG_WARNING("Connecting to ", attempt_.realm, " failed (", why, "); attempt ",
                attempt_.number + 1, " of ", kMaxAttempts, " in ", attempt_.retryIn, " s");
}

void UIScreenCallbackHandler::startRetry() {
    attempt_.retryPending = false;
    ++attempt_.number;
    attempt_.lastState = -1;
    attempt_.phaseTime = 0.0f;
    // Ask for the realm list again first, as the original client does: the
    // realm's address can change between attempts.
    if (authHandler_.isConnected()) {
        retryRealmName_ = attempt_.realm;
        authHandler_.requestRealmList();
        return;
    }
    connectToRealm_(attempt_.realm, attempt_.address);
}

bool UIScreenCallbackHandler::beginReconnect(uint64_t characterGuid) {
    if (lastRealm_.empty() || characterGuid == 0) return false;
    if (!uiManager_.getAuthScreen().hasCredentials()) {
        LOG_WARNING("Not reconnecting: no saved credentials to log in with");
        return false;
    }
    reconnect_ = {};
    reconnect_.active = true;
    reconnect_.characterGuid = characterGuid;
    LOG_WARNING("World connection lost; reconnecting to ", lastRealm_);
    startReconnectRound();
    return true;
}

void UIScreenCallbackHandler::cancelReconnect() {
    if (!reconnect_.active) return;
    LOG_INFO("Reconnect cancelled");
    reconnect_ = {};
    uiManager_.getAuthScreen().setStatusContext("");
    uiManager_.getCharacterScreen().cancelAutoEnter();
}

void UIScreenCallbackHandler::startReconnectRound() {
    ++reconnect_.round;
    reconnect_.roundTime = 0.0f;
    reconnect_.waiting = false;
    const std::string context = "Connection to " + lastRealm_ + " lost. Reconnecting (try " +
                                std::to_string(reconnect_.round) + " of " +
                                std::to_string(kMaxReconnectRounds) + ")...";
    LOG_WARNING(context);
    uiManager_.getAuthScreen().requestReconnect(context);
}

void UIScreenCallbackHandler::updateReconnect(float deltaTime) {
    auto& auth = uiManager_.getAuthScreen();
    if (reconnect_.waiting) {
        reconnect_.waitIn -= deltaTime;
        const int wait = static_cast<int>(std::max(0.0f, reconnect_.waitIn) + 0.99f);
        auth.setStatusContext("");
        auth.setStatus("Could not reconnect to " + lastRealm_ + ": " + reconnect_.lastError +
                           "\nTrying again in " + std::to_string(wait) + " s (try " +
                           std::to_string(reconnect_.round + 1) + " of " +
                           std::to_string(kMaxReconnectRounds) + "). Log In to try now.",
                       false, true);
        if (auth.isAuthenticating()) {
            // The player pressed Log In: that is this round, started early.
            reconnect_.waiting = false;
            ++reconnect_.round;
            reconnect_.roundTime = 0.0f;
            return;
        }
        if (reconnect_.waitIn <= 0.0f) startReconnectRound();
        return;
    }

    reconnect_.roundTime += deltaTime;
    using game::WorldState;
    const WorldState world = gameHandler_.getState();
    if (world == WorldState::CHAR_LIST_RECEIVED || world == WorldState::ENTERING_WORLD ||
        world == WorldState::IN_WORLD) {
        // In: the character screen takes it from the list into the world.
        LOG_INFO("Reconnected to ", lastRealm_, " on try ", reconnect_.round);
        reconnect_ = {};
        auth.setStatusContext("");
        return;
    }

    // Still going: logging in, waiting for the realm list, or in the watched
    // world connection, which shows its own steps on the character screen.
    const bool worldConnecting = world != WorldState::DISCONNECTED && world != WorldState::FAILED;
    if (auth.reconnectPending() || auth.isAuthenticating() || attempt_.active ||
        !retryRealmName_.empty() || worldConnecting || reconnect_.roundTime < 1.0f) {
        return;
    }

    // This round got nowhere: what it said last is on the login screen.
    std::string why = auth.getStatusMessage();
    if (const auto nl = why.rfind('\n'); nl != std::string::npos) why = why.substr(nl + 1);
    reconnect_.lastError = why.empty() ? std::string("no answer") : why;
    if (reconnect_.round >= kMaxReconnectRounds) {
        LOG_ERROR("Could not reconnect to ", lastRealm_, " after ", kMaxReconnectRounds,
                  " tries: ", reconnect_.lastError);
        reconnect_ = {};
        auth.setStatusContext("");
        auth.setStatus("Could not reconnect to " + lastRealm_ + " after " +
                           std::to_string(kMaxReconnectRounds) + " tries: " + why +
                           "\nThe server may be down. Log In to try again.",
                       true, true);
        return;
    }
    reconnect_.waiting = true;
    // Longer each time: a server restarting needs a minute, not a second.
    reconnect_.waitIn = 5.0f * static_cast<float>(reconnect_.round);
    LOG_WARNING("Reconnect try ", reconnect_.round, " failed (", reconnect_.lastError,
                "); next in ", reconnect_.waitIn, " s");
}

void UIScreenCallbackHandler::update(float deltaTime) {
    if (reconnect_.active) updateReconnect(deltaTime);
    if (!attempt_.active) return;
    const std::string of = attempt_.number > 1
        ? " (attempt " + std::to_string(attempt_.number) + " of " + std::to_string(kMaxAttempts) + ")"
        : std::string();

    if (attempt_.retryPending) {
        attempt_.retryIn -= deltaTime;
        char wait[32];
        std::snprintf(wait, sizeof(wait), "%d", static_cast<int>(std::max(0.0f, attempt_.retryIn) + 0.99f));
        connectionStatus_ = "Could not connect to " + attempt_.realm + ": " + attempt_.lastError +
                            ". Trying again in " + wait + " s (attempt " +
                            std::to_string(attempt_.number + 1) + " of " +
                            std::to_string(kMaxAttempts) + ")...";
        if (attempt_.retryIn <= 0.0f) startRetry();
        return;
    }
    if (!retryRealmName_.empty()) {
        // Waiting for the refreshed realm list.
        attempt_.phaseTime += deltaTime;
        connectionStatus_ = "Asking the login server where " + attempt_.realm + " is" + of + "...";
        if (attempt_.phaseTime > 6.0f) {
            // No list came back; use the address already known.
            retryRealmName_.clear();
            connectToRealm_(attempt_.realm, attempt_.address);
        }
        return;
    }

    using game::WorldState;
    const WorldState state = gameHandler_.getState();
    if (static_cast<int>(state) != attempt_.lastState) {
        attempt_.lastState = static_cast<int>(state);
        attempt_.phaseTime = 0.0f;
    } else {
        attempt_.phaseTime += deltaTime;
    }
    const int seconds = static_cast<int>(attempt_.phaseTime);

    switch (state) {
        case WorldState::CHAR_LIST_RECEIVED:
        case WorldState::ENTERING_WORLD:
        case WorldState::IN_WORLD:
            if (attempt_.number > 1) {
                LOG_INFO("Connected to ", attempt_.realm, " on attempt ", attempt_.number);
            }
            attempt_ = {};
            connectionStatus_.clear();
            return;
        case WorldState::CONNECTING:
        case WorldState::CONNECTED:
            connectionStatus_ = "Connecting to " + attempt_.realm + of + "...";
            // The server greets a connection within a fraction of a second;
            // one it has not greeted in this long it never will.
            if (attempt_.phaseTime > 6.0f) scheduleRetry("the world server did not answer");
            return;
        case WorldState::CHALLENGE_RECEIVED:
            connectionStatus_ = "Signing in to " + attempt_.realm + of + "...";
            if (attempt_.phaseTime > 6.0f) scheduleRetry("the world server did not answer");
            return;
        case WorldState::AUTH_SENT:
            // No deadline: the server may be holding this account in its queue.
            connectionStatus_ = attempt_.phaseTime < 5.0f
                ? "Signing in to " + attempt_.realm + of + "..."
                : "Waiting for " + attempt_.realm + " to let this account in (" +
                      std::to_string(seconds) + " s)" + of + "...";
            return;
        case WorldState::AUTHENTICATED:
        case WorldState::READY:
        case WorldState::CHAR_LIST_REQUESTED:
            connectionStatus_ = attempt_.phaseTime < 3.0f
                ? std::string("Signed in. Loading characters...")
                : "Signed in. The server is checking the client before listing characters (" +
                      std::to_string(seconds) + " s)...";
            if (attempt_.phaseTime > 60.0f) scheduleRetry("no character list after a minute");
            return;
        case WorldState::DISCONNECTED:
        case WorldState::FAILED:
            scheduleRetry("the world server closed the connection");
            return;
    }
}

}} // namespace wowee::core
