#pragma once

#include "core/event_bus.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class FrameXmlEvents;

inline constexpr int kActionBarSlotsPerBar = 12;
inline constexpr int kActionBars = 12;
inline constexpr int kActionBarSlots = kActionBarSlotsPerBar * kActionBars;  // 144

using ActionBar = std::array<ActionBarSlot, kActionBarSlots>;

/// A cooldown as a button draws it: seconds left, and how long it was when it
/// began. Both zero when the action is ready.
struct ActionCooldown {
    float remaining = 0.0f;
    float total = 0.0f;
    [[nodiscard]] bool ready() const { return remaining <= 0.0f; }
};

/// What the action bar asks of the rest of the game about the actions on it.
class ActionBarQueries {
public:
    virtual ~ActionBarQueries() = default;

    /// A spell's cooldown; zero when it is ready.
    [[nodiscard]] virtual ActionCooldown spellCooldown(uint32_t spellId) const = 0;
    /// The cooldown of the spell an item casts when used; zero when it is
    /// ready or the item is not known yet.
    [[nodiscard]] virtual ActionCooldown itemCooldown(uint32_t itemId) const = 0;
    /// Ask for an item's description, so a button holding it can show its
    /// name and icon.
    virtual void requestItemInfo(uint32_t itemId) = 0;
    /// The character's PLAYER_FIELD_BYTES as the realm last sent it. Empty
    /// before it has, or when this expansion's field table does not name the
    /// field.
    [[nodiscard]] virtual std::optional<uint32_t> playerFieldBytes() const = 0;
};

/// The character's action bar: 144 buttons, twelve bars of twelve.
///
/// The server's to say. It keeps the bar per character, sends the whole of it
/// at every login (SMSG_ACTION_BUTTONS, with the race and class defaults for
/// a new character) and is told of every button the player changes
/// (CMSG_SET_ACTION_BUTTON). Nothing of it is kept here between sessions.
///
/// Not account data: the account data's macros are what a macro button
/// names, and when one is deleted (events::MacroDeleted) the buttons holding
/// it are emptied here. Nor the spell book: a button's cooldown is the spell's
/// own, asked for when it is drawn, and a spell unlearned or replaced by its
/// next rank (events::SpellsUnlearned, SpellSuperseded) changes the buttons
/// that held it.
class ActionBarHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    ActionBarHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                     core::EventBus& events, ActionBarQueries& queries);

    void registerOpcodes(DispatchTable& table);

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] const ActionBar& slots() const { return bar_; }
    /// The cooldown of what a button holds, as the spell book has it.
    [[nodiscard]] ActionCooldown cooldown(int slot) const;

    /// Which of the four extra bars are shown - bottom left, bottom right,
    /// right and right 2, as bits 0 to 3. The realm keeps them in byte 2 of
    /// the character's PLAYER_FIELD_BYTES and sends them with it, so the bars
    /// another client turned on are on here. Empty while that is not known.
    [[nodiscard]] std::optional<uint8_t> extraBarsShown() const;

    // --- Commands ------------------------------------------------------------

    /// Put an action on a button, or empty it (EMPTY, 0): drawn at once and
    /// sent to the server once a character is in the world.
    void setSlot(int slot, ActionBarSlot::Type type, uint32_t id);
    /// Show or hide the extra bars: CMSG_SET_ACTIONBAR_TOGGLES, answered from
    /// here until the next login rather than waiting for the realm's echo.
    void setExtraBarsShown(uint8_t mask);
    /// A character is being logged in; its bar arrives from the server.
    void clear();

private:
    void handleActionButtons(network::Packet& packet);
    void removeMacro(uint32_t macroId);
    /// Spells the character no longer knows come off the buttons.
    void forgetSpells(const std::vector<uint32_t>& spellIds);
    /// A spell's next rank takes its place on the buttons.
    void replaceSpell(uint32_t oldSpellId, uint32_t newSpellId);

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    ActionBarQueries& queries_;
    ActionBar bar_{};
    /// The extra bars as this client last set them for this character.
    std::optional<uint8_t> extraBarsSetHere_;
    /// That the realm's extra bars could not be read has been said.
    mutable bool extraBarsUnknownSaid_ = false;
    /// Action types already reported as not drawn, so each is said once.
    std::unordered_set<uint8_t> reportedTypes_;
    /// Last, so it goes first: its handler writes the bar.
    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace wowee::game
