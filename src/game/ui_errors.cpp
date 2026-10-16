#include "game/ui_errors.hpp"

#include "core/event_bus.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_events.hpp"

namespace wowee::game {

void raiseUiError(core::EventBus& bus, FrameXmlEvents& frameXml,
                  const std::string& message) {
    bus.publish(events::UiErrorRaised{.message = message});
    frameXml.fireFrameXmlEvent("UI_ERROR_MESSAGE", {message});
}

} // namespace wowee::game
