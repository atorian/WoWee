#pragma once

#include <cstdint>
#include <string>

namespace wowee::game {

enum class ChatType : uint8_t;

/// The chat log, as a domain that has something to say to the player sees it:
/// "Received item: [Linen Cloth]", "Looted: 3 silver", "You have learned a new
/// spell". A line here is the game's own, not a message from anyone - it is
/// drawn in the chat frame and announced to FrameXML like one.
///
/// ChatHandler owns the log and implements this; a domain that writes to it
/// takes this rather than the handler.
class ChatLog {
public:
    virtual ~ChatLog() = default;

    /// A yellow system line, CHAT_MSG_SYSTEM.
    virtual void addSystemChatMessage(const std::string& message) = 0;
    /// A line of another kind the game composes itself - the money line after
    /// a loot is CHAT_MSG_MONEY, not a system message.
    virtual void addLocalChatLine(ChatType type, const std::string& message) = 0;
};

} // namespace wowee::game
