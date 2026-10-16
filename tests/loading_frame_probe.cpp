#include "loading_frame_probe.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/input.hpp"
#include "core/coordinates.hpp"
#include "core/entity_spawner.hpp"
#include "game/game_handler.hpp"
#include "game/game_events.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/camera_controller.hpp"
#include <cstdlib>

namespace wowee::core {
namespace { struct PostedLoadingProbe {}; }

LoadingFrameProbe::LoadingFrameProbe(Application& app, EventBus& events, std::string action)
    : app_(app), events_(events), action_(std::move(action)) {
    waitStage_ = std::getenv("WOWEE_LOADING_TEST_STAGE") ? std::getenv("WOWEE_LOADING_TEST_STAGE") : "warmup";
    if (!std::getenv("WOWEE_MULGORE_PROBE") ||
        (action_ != "close" && action_ != "disconnect" && action_ != "logout" &&
         action_ != "transfer" && action_ != "release" && action_ != "resize" && action_ != "ready" &&
         action_ != "entry-spawn")) {
        fail("requires offline fixture and a supported action");
    }
    subscriptions_.push_back(events_.subscribe<PostedLoadingProbe>([this](const auto&) { posted_ = true; }));
}

void LoadingFrameProbe::frameBegan() { ++frame_; }

void LoadingFrameProbe::pass(const char* detail) {
    if (done_) return;
    // The action does not wait for this: a stage can end before the
    // interface has had a frame to show it. An action that keeps the client
    // loading must still have seen loading run OnUpdate, deliver the game's
    // notifications in order and dispatch posted events by the time it
    // succeeds. One that leaves at once (close, disconnect, logout) can end
    // the session before the interface has a loading frame; that work is
    // covered by the client_transitions ctest instead.
    const bool leavesAtOnce = action_ == "close" || action_ == "disconnect" || action_ == "logout";
    const bool needsInterfaceEvidence = !leavesAtOnce && action_ != "entry-spawn";
    if (needsInterfaceEvidence && (!luaVerified_ || !posted_)) {
        fail("interface/events work during loading was not observed");
        return;
    }
    done_ = true;
    const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - actionAt_).count();
    LOG_WARNING("Loading probe PASS: ", action_, " ", detail, " frames=", frame_ - actionFrame_, " latency_ms=", ms);
    app_.getWindow()->setShouldClose(true);
}

void LoadingFrameProbe::fail(const char* detail) {
    if (done_) return;
    done_ = true;
    LOG_ERROR("Loading probe FAIL: ", action_, " ", detail);
    app_.getWindow()->setShouldClose(true);
}

void LoadingFrameProbe::streamingDone(const FramePlan& plan) {
    if (done_) return;
    auto* loader = app_.getWorldLoader();
    // A real network burst can contain the first create blocks before the
    // client's entry transition. Observe the visible result before any old
    // periodic repair scan can hide a discarded notification.
    constexpr uint64_t entryGuid = 0xF130000001000042ULL;
    if (action_ == "entry-spawn" && plan.world && !entrySpawnQueued_) {
        entrySpawnQueued_ = true;
        const auto display = app_.getEntitySpawner()->getGryphonDisplayId();
        if (!display) { fail("fixture has no gryphon display"); return; }
        auto entity = std::make_shared<game::Unit>(entryGuid);
        const glm::vec3 destination(-8549.58f, 1003.11f, 89.7306f);
        const auto pos = coords::serverToCanonical(destination);
        entity->setPosition(pos.x + 4, pos.y, pos.z, 0);
        entity->setDisplayId(display);
        app_.getGameHandler()->getEntityManager().addEntity(entryGuid, entity);
        app_.getEntitySpawner()->queueCreatureSpawn(entryGuid, display, pos.x + 4, pos.y, pos.z, 0);
        app_.getGameHandler()->currentMapIdRef() = 0;
        app_.getGameHandler()->setPosition(destination.x, destination.y, destination.z);
        events_.post(game::events::WorldEntered{0, destination.x, destination.y, destination.z, false});
    }
    if (action_ == "entry-spawn" && entrySpawnQueued_ && plan.loading) inWorld_ = true;
    if (action_ == "entry-spawn" && inWorld_ && plan.world) {
        if (app_.getEntitySpawner()->getCreatureInstanceId(entryGuid)) pass("early entry spawn is visible without a repair scan");
        else fail("entry transition discarded the first creature spawn");
        return;
    }
    if (!fired_ && installed_ && app_.getState() == AppState::AUTHENTICATION) {
        fail("world load failed before the requested action"); return;
    }
    if (std::chrono::duration<float>(Clock::now() - started_).count() > 90) { fail("timed out"); return; }
    if (plan.loading && installed_ && !sentEvents_) {
        sentEvents_ = true;
        events_.post(PostedLoadingProbe{});
        app_.getGameHandler()->fireAddonEvent("WOWEE_LOADING_PROBE", {"one"});
        app_.getGameHandler()->fireAddonEvent("WOWEE_LOADING_PROBE", {"two"});
    }
    if (action_ == "release" && plan.loading) {
        if (!keyHeld_) {
            keyHeld_ = true;
            actionFrame_ = frame_;
            actionAt_ = Clock::now();
            Input::getInstance().setVirtualKey(SDL_SCANCODE_W, true);
        } else if (!keyReleased_ && frame_ > actionFrame_) {
            keyReleased_ = true;
            Input::getInstance().setVirtualKey(SDL_SCANCODE_W, false);
            LOG_WARNING("Loading probe: released forward key while loading");
        }
    }
    const bool atStage = (waitStage_ == "warmup" && loader->stage() == WorldLoader::Stage::Warmup) ||
                         (waitStage_ == "preparing" && loader->stage() == WorldLoader::Stage::Preparing) ||
                         (waitStage_ == "geometry" && loader->stage() == WorldLoader::Stage::Geometry);
    if (!fired_ && plan.loading && atStage) {
        fired_ = true;
        actionFrame_ = frame_;
        actionAt_ = Clock::now();
        LOG_WARNING("Loading probe action: ", action_, " frame=", frame_);
        if (action_ == "close") {
            SDL_Event event{};
            event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            event.window.windowID = SDL_GetWindowID(app_.getWindow()->getSDLWindow());
            if (!SDL_PushEvent(&event)) fail("could not post window-close input");
        } else if (action_ == "resize") {
            app_.getWindow()->setFullscreen(false);
            if (!SDL_SetWindowSize(app_.getWindow()->getSDLWindow(), targetSize_.x, targetSize_.y))
                fail("could not resize window");
        } else if (action_ == "disconnect") events_.post(game::events::Disconnected{});
        else if (action_ == "logout") events_.post(game::events::LogoutCompleted{});
        else if (action_ == "transfer") {
            const auto entry = loader->entry();
            worldPosition_ = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(entry.x + 100, entry.y, entry.z)));
            app_.getGameHandler()->setPosition(entry.x + 100, entry.y, entry.z);
            events_.post(game::events::WorldEntered{entry.mapId, entry.x + 100, entry.y, entry.z, false});
        }
    }
    if (fired_ && action_ == "resize" && frame_ > actionFrame_ + 2) {
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(app_.getWindow()->getSDLWindow(), &width, &height);
        const auto extent = app_.getWindow()->getVkContext()->getSwapchainExtent();
        if (app_.getWindow()->getWidth() == targetSize_.x && app_.getWindow()->getHeight() == targetSize_.y &&
            extent.width == static_cast<uint32_t>(width) && extent.height == static_cast<uint32_t>(height))
            pass("resize rebuilt the swapchain during loading");
    }
    if (fired_ && plan.world && action_ == "ready" && ++enteredFrames_ == 3)
        pass("world entered and rendered");
    if (fired_ && (action_ == "disconnect" || action_ == "logout") && app_.getState() == AppState::AUTHENTICATION) {
        const auto elapsed = std::chrono::duration<float>(Clock::now() - actionAt_).count();
        if (elapsed >= 1) { fail("return to login exceeded one second"); return; }
        if (++loginFrames_ == 3) pass("returned to login");
    }
    if (fired_ && plan.world && action_ == "transfer") {
        if (glm::length(glm::vec2(app_.getRenderer()->getCharacterPosition() - worldPosition_)) > 5)
            fail("entered the stale destination");
        else pass("replacement destination entered");
    }
    if (fired_ && plan.world && action_ == "release") {
        if (!inWorld_) {
            inWorld_ = true;
            worldAt_ = Clock::now();
            worldPosition_ = app_.getRenderer()->getCharacterPosition();
        }
        if (std::chrono::duration<float>(Clock::now() - worldAt_).count() >= 4) {
            const auto delta = app_.getRenderer()->getCharacterPosition() - worldPosition_;
            if (!keyReleased_ || app_.getCameraController()->isMoving() || glm::length(glm::vec2(delta)) > 0.25f)
                fail("character kept moving after key release");
            else pass("released key did not move the character");
        }
    }
}

void LoadingFrameProbe::interfaceStarting(const FramePlan& plan) {
    if (done_ || !plan.loading) return;
    auto* addons = app_.getAddonManager();
    if (!addons || !addons->getLuaEngine()) { fail("no interface engine"); return; }
    if (!installed_) {
        installed_ = true;
        if (!addons->getLuaEngine()->executeString(R"lua(
            WOWEE_LOAD_UPDATES = 0; WOWEE_LOAD_EVENTS = ''
            local f = CreateFrame('Frame')
            f:RegisterEvent('WOWEE_LOADING_PROBE')
            f:SetScript('OnEvent', function(_, _, arg) WOWEE_LOAD_EVENTS = WOWEE_LOAD_EVENTS .. arg end)
            f:SetScript('OnUpdate', function() WOWEE_LOAD_UPDATES = WOWEE_LOAD_UPDATES + 1 end)
        )lua")) { fail("could not install interface observer"); return; }
    }
    if (!luaVerified_ && posted_ && addons->interfaceCommandBoolean("WOWEE_LOAD_UPDATES > 0 and WOWEE_LOAD_EVENTS == 'onetwo'")) {
        luaVerified_ = true;
        LOG_WARNING("Loading probe: OnUpdate, ordered game events and posted events delivered during loading");
    }
}

void LoadingFrameProbe::loopEnded() {
    if (done_) return;
    if (action_ == "close" && fired_ && frame_ == actionFrame_ + 1)
        pass("window close handled on next Input frame");
    else fail("client loop ended before the scenario completed");
}
}
