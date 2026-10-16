#pragma once

namespace wowee::network { class Packet; }

namespace wowee::game {

/// Where a domain handler sends what it has to tell the world server.
///
/// The session owns the connection and knows how far the login has got; a
/// handler needs only to send and to know whether sending makes sense yet.
/// GameHandler implements it over its network::WorldConnection; a test can
/// hand over its own.
class PacketSender {
public:
    virtual ~PacketSender() = default;

    /// Whether a world server connection is up to send on.
    [[nodiscard]] virtual bool connected() const = 0;
    /// Whether a character is in the world. What the server stores against a
    /// character - its action bar, its account data - waits for this: until
    /// the server has loaded the character it has nowhere to put it.
    [[nodiscard]] virtual bool characterInWorld() const = 0;
    /// Whether a command the player asked for that needs the server - a cast,
    /// loot, trade, chat, an NPC - can go now: a character in the world, on a
    /// connection. Asked while the world connection is lost and being
    /// restored, it is refused at once and the player told so; the caller
    /// keeps nothing to send once the connection is back. `command` names it
    /// in the log.
    virtual bool worldCommandAllowed(const char* command) = 0;
    /// Send on the world connection. With no connection the packet is
    /// dropped; ask connected() first where that matters.
    virtual void send(const network::Packet& packet) = 0;
};

} // namespace wowee::game
