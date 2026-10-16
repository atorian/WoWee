#pragma once

#include <string>
#include <vector>

namespace wowee::game {

/// The FrameXML event channel: "UPDATE_MACROS", "ACTIONBAR_SLOT_CHANGED" and
/// the rest, with their string arguments. This is the addon contract and the
/// names stay strings; it is a separate channel from core::EventBus, which
/// carries the game's notifications to the client.
///
/// GameHandler implements it over its AddonEventQueue: an event fired here is
/// queued and reaches FrameXML in the next Interface phase, and is dropped
/// when no interface is listening.
class FrameXmlEvents {
public:
    virtual ~FrameXmlEvents() = default;

    virtual void fireFrameXmlEvent(const std::string& name,
                                   const std::vector<std::string>& args) = 0;
};

} // namespace wowee::game
