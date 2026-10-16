#pragma once

#include "core/event_bus.hpp"
#include "game/game_events.hpp"

#include <array>
#include <algorithm>
#include <deque>
#include <optional>
#include <variant>

namespace wowee::core {

/// Event subscribers only record requests. ClientLoop consumes them once,
/// after submitting the frame. A leave supersedes any queued world entry.
class WorldTransitionQueue {
public:
    // Explicit login/exit requests take precedence over automatic reconnect.
    enum class Destination { CharacterSelect, Reconnect, Login, Exit };
    struct Leave {
        Destination destination;
        uint64_t characterGuid = 0;
    };
    using Request = std::variant<Leave, game::events::WorldTransferStarted,
                                 game::events::WorldEntered>;

    explicit WorldTransitionQueue(EventBus& bus) {
        subscriptions_[0] = bus.subscribe<game::events::LogoutCompleted>([this](const auto& e) {
            leave(e.exiting ? Destination::Exit : Destination::CharacterSelect);
        });
        subscriptions_[1] = bus.subscribe<game::events::Disconnected>([this](const auto& e) {
            leave(Destination::Reconnect, e.characterGuid);
        });
        subscriptions_[2] = bus.subscribe<game::events::WorldTransferStarted>([this](const auto& e) {
            if (!leaving()) requests_.emplace_back(e);
        });
        subscriptions_[3] = bus.subscribe<game::events::WorldEntered>([this](const auto& e) {
            if (!leaving()) requests_.emplace_back(e);
        });
        subscriptions_[4] = bus.subscribe<game::events::ExitRequested>([this](const auto&) {
            leave(Destination::Exit);
        });
    }

    void leave(Destination destination, uint64_t characterGuid = 0) {
        if (leaving() && std::get<Leave>(requests_.front()).destination > destination) return;
        requests_.clear();
        requests_.emplace_back(Leave{destination, characterGuid});
    }

    [[nodiscard]] bool empty() const { return requests_.empty(); }
    [[nodiscard]] bool leaving() const {
        return !empty() && std::holds_alternative<Leave>(requests_.front());
    }
    /// The queued leave; nothing when none is queued.
    [[nodiscard]] std::optional<Leave> queuedLeave() const {
        if (!leaving()) return std::nullopt;
        return std::get<Leave>(requests_.front());
    }
    [[nodiscard]] bool hasWorldChange() const {
        return std::any_of(requests_.begin(), requests_.end(), [](const auto& request) {
            return !std::holds_alternative<game::events::WorldTransferStarted>(request);
        });
    }
    std::optional<Request> takeNext() {
        if (empty()) return std::nullopt;
        auto next = std::move(requests_.front());
        requests_.pop_front();
        return next;
    }

private:
    std::deque<Request> requests_;
    std::array<EventBus::Subscription, 5> subscriptions_;
};

} // namespace wowee::core
