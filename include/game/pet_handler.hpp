#pragma once

#include "game/opcode_table.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::core { class EventBus; }
namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class FrameXmlEvents;
class ChatLog;

/// A pet kept at a stable master, as MSG_LIST_STABLED_PETS lists it.
struct StabledPet {
    uint32_t petNumber  = 0;   // server-side pet number (used for unstable/swap)
    uint32_t entry      = 0;   // creature entry ID
    uint32_t level      = 0;
    std::string name;
    // No display id: MSG_LIST_STABLED_PETS carries only the entry, and a
    // model frame wants a display id. SetPetStablePaperdoll resolves it from
    // `entry` at the moment it draws, so a query still in flight answers on
    // the next read rather than being frozen at zero.
    bool     isActive   = false;  // true = the pet that is out, not stabled
};

/// What the pet asks of the rest of the game.
class PetQueries {
public:
    virtual ~PetQueries() = default;

    [[nodiscard]] virtual std::string spellName(uint32_t spellId) const = 0;
    /// Give a unit in view the name its owner chose. False when it is not in
    /// view, or not a unit.
    virtual bool nameUnitInView(uint64_t guid, const std::string& name) = 0;
};

/// The player's pet and the pets kept at a stable master.
///
/// Which pet is out, its ten-button bar, the spells it knows and which of
/// them it casts by itself, what it is told to do and how it reacts, its own
/// stats off its unit fields, what its owner named it, and the stable. All of
/// it is the server's to say: SMSG_PET_SPELLS at summon, SMSG_PET_MODE as the
/// player commands it, SMSG_PET_BROKEN when it is gone.
class PetHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    static constexpr int kActionBarSlots = 10;

    PetHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
               ChatLog& chat, PetQueries& queries);

    void registerOpcodes(DispatchTable& table);

    /// A character is being logged in: the last one's pet and stable are not
    /// this one's.
    void beginCharacter();

    // ---- The pet --------------------------------------------------------------

    [[nodiscard]] uint64_t guid() const { return held_.guid; }
    [[nodiscard]] bool hasPet() const { return held_.guid != 0; }
    [[nodiscard]] uint32_t actionSlot(int index) const {
        if (index < 0 || index >= kActionBarSlots) return 0;
        return held_.actionSlots[static_cast<size_t>(index)];
    }
    /// 0 stay, 1 follow, 2 attack, 3 dismiss.
    [[nodiscard]] uint8_t command() const { return held_.command; }
    /// 0 passive, 1 defensive, 2 aggressive.
    [[nodiscard]] uint8_t react() const { return held_.react; }
    [[nodiscard]] const std::vector<uint32_t>& spells() const { return held_.spells; }
    [[nodiscard]] bool isAutocast(uint32_t spellId) const { return held_.autocast.count(spellId) != 0; }
    /// The server offered to name a newly tamed pet (SMSG_PET_RENAMEABLE).
    [[nodiscard]] bool renameable() const { return held_.renameable; }

    // The pet's own stats, off its unit fields. Armor is resistance zero.
    [[nodiscard]] const std::array<int32_t, 5>& stats() const { return held_.stats; }
    [[nodiscard]] const std::array<int32_t, 7>& resistances() const { return held_.resistances; }
    [[nodiscard]] int32_t attackPower() const { return held_.attackPower; }
    [[nodiscard]] float minDamage() const { return held_.minDamage; }
    [[nodiscard]] float maxDamage() const { return held_.maxDamage; }
    [[nodiscard]] uint32_t experience() const { return held_.experience; }
    [[nodiscard]] uint32_t nextLevelExperience() const { return held_.nextLevelExperience; }

    /// The pet unit's fields, as the realm sends them for the player's pet.
    void setAttackPower(int32_t value) { held_.attackPower = value; }
    void setMinDamage(float value) { held_.minDamage = value; }
    void setMaxDamage(float value) { held_.maxDamage = value; }
    void setStat(int index, int32_t value);
    void setResistance(int index, int32_t value);
    /// Returns whether the experience changed, which the caller announces.
    bool setExperience(uint32_t value);
    bool setNextLevelExperience(uint32_t value);

    // ---- Commanding it --------------------------------------------------------

    /// CMSG_PET_ACTION: a button on the pet bar, or a command.
    void sendAction(uint32_t action, uint64_t targetGuid = 0);
    /// Put the pet away (COMMAND_ABANDON as a pet action - a hunter calls it back).
    void dismiss();
    void toggleAutocast(uint32_t spellId);
    void rename(const std::string& newName);
    /// Give a hunter's pet up for good. The interface asks first.
    void abandon();

    // ---- Pet talents ------------------------------------------------------------

    [[nodiscard]] bool showUnlearnDialog() const { return unlearnPending_; }
    [[nodiscard]] uint32_t unlearnCost() const { return unlearnCost_; }
    void confirmUnlearn();
    void cancelUnlearn() { unlearnPending_ = false; }

    // ---- The stable -------------------------------------------------------------

    [[nodiscard]] bool isStableWindowOpen() const { return stable_.windowOpen; }
    void closeStableWindow();
    [[nodiscard]] uint8_t stableSlots() const { return stable_.numSlots; }
    [[nodiscard]] const std::vector<StabledPet>& stabledPets() const { return stable_.pets; }
    /// The player is talking to a stable master; the window opens when the
    /// list it asks for arrives.
    void setStableMaster(uint64_t npcGuid);
    void requestStabledPetList();
    void stablePet(uint8_t slot);
    void unstablePet(uint32_t petNumber);
    /// Buy the next stable slot from the stable master currently open.
    void buyStableSlot();

private:
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);
    void handleStableResult(network::Packet& packet);
    void handlePetNameQueryResponse(network::Packet& packet);
    void handleTameFailure(network::Packet& packet);
    void handleActionFeedback(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleBroken(network::Packet& packet);
    void handleLearnedSpell(network::Packet& packet);
    void handleUnlearnedSpell(network::Packet& packet);
    void handleMode(network::Packet& packet);

    /// The pet is gone: its bar and spells with it, and the interface told.
    void clearPet();
    /// The pet frame and the pet action bar both rebuild from these.
    void announcePetChanged();
    /// Ask the server what the pet is called. The name a player gave a pet
    /// arrives only in answer to CMSG_PET_NAME_QUERY; without it the pet wears
    /// its creature template's name.
    void requestName(uint64_t petGuid);
    void raiseUiError(const std::string& message);

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    PetQueries& queries_;

    /// The character's pet, assigned whole by beginCharacter.
    struct Held {
        uint64_t guid = 0;
        std::array<uint32_t, kActionBarSlots> actionSlots{};
        uint8_t command = 1;
        uint8_t react = 1;
        bool renameable = false;
        std::vector<uint32_t> spells;
        std::unordered_set<uint32_t> autocast;
        std::array<int32_t, 5> stats{};
        int32_t attackPower = 0;
        float minDamage = 0.0f;
        float maxDamage = 0.0f;
        std::array<int32_t, 7> resistances{};
        uint32_t experience = 0;
        uint32_t nextLevelExperience = 0;
    };
    Held held_;

    struct Stable {
        bool windowOpen = false;
        uint64_t masterGuid = 0;
        uint8_t numSlots = 0;
        std::vector<StabledPet> pets;
    };
    Stable stable_;

    bool unlearnPending_ = false;
    uint32_t unlearnCost_ = 0;

    /// CMSG_PET_NAME_QUERY carries a key the server echoes back; this matches
    /// the reply to the pet it asked about.
    std::unordered_map<uint32_t, uint64_t> pendingNameQueries_;
    uint32_t nextNameQueryKey_ = 1;
};

} // namespace wowee::game
