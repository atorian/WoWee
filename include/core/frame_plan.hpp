#pragma once

namespace wowee::core {

enum class AppState {
    AUTHENTICATION, REALM_SELECTION, CHARACTER_CREATION, CHARACTER_SELECTION,
    IN_GAME, DISCONNECTED, FIRST_RUN, LOADING_WORLD
};

enum class Phase { Idle, Input, Network, Game, Events, Streaming, Interface, Present, Render, Transitions, Pace };
enum class FrameView { Login, Realms, CharacterCreation, Characters, World, Disconnected, FirstRun, Loading };

struct FramePlan {
    bool authNetwork = false;
    bool worldNetwork = false;
    bool game = false;
    bool world = false;
    bool loading = false;
    FrameView view = FrameView::Login;
};

constexpr FramePlan framePlanFor(AppState state) {
    switch (state) {
        case AppState::AUTHENTICATION: return {.authNetwork = true, .view = FrameView::Login};
        case AppState::REALM_SELECTION: return {.authNetwork = true, .view = FrameView::Realms};
        case AppState::CHARACTER_CREATION: return {.worldNetwork = true, .game = true, .view = FrameView::CharacterCreation};
        case AppState::CHARACTER_SELECTION: return {.worldNetwork = true, .game = true, .view = FrameView::Characters};
        case AppState::IN_GAME: return {.worldNetwork = true, .game = true, .world = true, .view = FrameView::World};
        case AppState::LOADING_WORLD: return {.worldNetwork = true, .game = true, .loading = true, .view = FrameView::Loading};
        case AppState::DISCONNECTED: return {.view = FrameView::Disconnected};
        case AppState::FIRST_RUN: return {.view = FrameView::FirstRun};
    }
    return {};
}

constexpr const char* phaseName(Phase phase) {
    switch (phase) {
        case Phase::Idle: return "Idle";
        case Phase::Input: return "Input";
        case Phase::Network: return "Network";
        case Phase::Game: return "Game";
        case Phase::Events: return "Events";
        case Phase::Streaming: return "Streaming";
        case Phase::Interface: return "Interface";
        case Phase::Present: return "Present";
        case Phase::Render: return "Render";
        case Phase::Transitions: return "Transitions";
        case Phase::Pace: return "Pace";
    }
    return "Unknown";
}

} // namespace wowee::core
