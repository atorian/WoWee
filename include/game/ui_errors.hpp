#pragma once

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::game {

class FrameXmlEvents;

/// Put a refusal on the player's error line - "Not enough mana", "That bag is
/// full" - and only there, which is where the real client puts these.
///
/// Two things answer it: the client (events::UiErrorRaised, which draws the
/// red line and sounds it) and FrameXML (UI_ERROR_MESSAGE, what UIErrorsFrame
/// and addons are registered for). Every domain raises its errors through
/// this, so the two cannot drift apart and nothing raises one twice.
void raiseUiError(core::EventBus& bus, FrameXmlEvents& frameXml,
                  const std::string& message);

} // namespace wowee::game
