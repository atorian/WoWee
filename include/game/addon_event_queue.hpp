#pragma once

#include <deque>
#include <string>
#include <variant>
#include <vector>

namespace wowee::game {

/// The game owns payloads until the client explicitly delivers them in Interface.
class AddonEventQueue {
public:
    void event(const std::string& name, const std::vector<std::string>& args) {
        pending_.emplace_back(Event{name, args});
    }
    void command(const std::string& code) { pending_.emplace_back(Command{code}); }

    template<class EventSink, class CommandSink>
    void dispatch(EventSink&& eventSink, CommandSink&& commandSink) {
        auto pending = std::move(pending_);
        pending_.clear();
        for (const auto& notification : pending) {
            if (const auto* event = std::get_if<Event>(&notification))
                eventSink(event->name, event->args);
            else
                commandSink(std::get<Command>(notification).code);
        }
    }

private:
    struct Event { std::string name; std::vector<std::string> args; };
    struct Command { std::string code; };
    std::deque<std::variant<Event, Command>> pending_;
};

} // namespace wowee::game
