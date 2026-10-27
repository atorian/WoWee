#include "game/pet_handler.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/game_utils.hpp"
#include "game/frame_xml_events.hpp"
#include "game/packet_sender.hpp"
#include "game/pet_gateway.hpp"
#include "game/spell_defines.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "pet/domain/records.hpp"
#include "pet/services/pet.hpp"

#include <utility>

namespace wowee::game {

PetHandler::PetHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                       ChatLog& chat, PetQueries& queries)
    : sender_(sender), frameXml_(frameXml), events_(events), chat_(chat), queries_(queries),
      subscriptions_(subscribePetNotices(events, chat, frameXml, queries)) {}

void PetHandler::raiseUiError(const std::string& message) {
    game::raiseUiError(events_, frameXml_, message);
}

pet::IPetFeed* PetHandler::feed() {
    auto* feed = queries_.petFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("The pet is not wired into this client; pet packets are dropped");
    return feed;
}

void PetHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_PET_SPELLS] = [this](network::Packet& p) { handlePetSpells(p); };
    table[Opcode::MSG_LIST_STABLED_PETS] = [this](network::Packet& p) {
        if (sender_.characterInWorld()) handleListStabledPets(p);
    };
    table[Opcode::SMSG_STABLE_RESULT] = [this](network::Packet& p) {
        if (!p.hasRemaining(1)) return;
        const uint8_t result = p.readUInt8();
        if (auto* pet = feed()) pet->onStableResult(result);
    };
    table[Opcode::SMSG_PET_NAME_QUERY_RESPONSE] = [this](network::Packet& p) {
        handlePetNameQueryResponse(p);
    };
    table[Opcode::SMSG_PET_TAME_FAILURE] = [this](network::Packet& p) { handleTameFailure(p); };
    table[Opcode::SMSG_PET_ACTION_FEEDBACK] = [this](network::Packet& p) { handleActionFeedback(p); };
    table[Opcode::SMSG_PET_CAST_FAILED] = [this](network::Packet& p) { handleCastFailed(p); };
    // Pet bond broken (died or forcibly dismissed).
    table[Opcode::SMSG_PET_BROKEN] = [this](network::Packet& p) {
        LOG_INFO("SMSG_PET_BROKEN: pet bond broken");
        if (auto* pet = feed()) pet->onPetLost();
        p.skipAll();
    };
    table[Opcode::SMSG_PET_LEARNED_SPELL] = [this](network::Packet& p) {
        if (p.hasRemaining(4)) {
            const uint32_t spellId = p.readUInt32();
            LOG_DEBUG("SMSG_PET_LEARNED_SPELL: spellId=", spellId);
            if (auto* pet = feed()) pet->onSpellLearned(spellId);
        }
        p.skipAll();
    };
    table[Opcode::SMSG_PET_UNLEARNED_SPELL] = [this](network::Packet& p) {
        if (p.hasRemaining(4)) {
            const uint32_t spellId = p.readUInt32();
            LOG_DEBUG("SMSG_PET_UNLEARNED_SPELL: spellId=", spellId);
            if (auto* pet = feed()) pet->onSpellUnlearned(spellId);
        }
        p.skipAll();
    };
    table[Opcode::SMSG_PET_MODE] = [this](network::Packet& p) { handleMode(p); };
    // Server signals that the pet can now be named (first tame)
    table[Opcode::SMSG_PET_RENAMEABLE] = [this](network::Packet& packet) {
        if (auto* pet = feed()) pet->onRenameable();
        packet.skipAll();
    };
    table[Opcode::SMSG_PET_NAME_INVALID] = [this](network::Packet& packet) {
        // Once: this went through both of GameHandler's names for the error
        // line, which put it up and sounded it twice.
        raiseUiError("That pet name is invalid. Please choose a different name.");
        packet.skipAll();
    };
    // SMSG_PET_UNLEARN_CONFIRM: uint64 petGuid + uint32 cost (copper). The
    // other pet opcodes have different formats and must NOT set unlearn state.
    table[Opcode::SMSG_PET_UNLEARN_CONFIRM] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            packet.readUInt64();   // the pet's guid; the cost is what is asked for
            const uint32_t cost = packet.readUInt32();
            if (auto* pet = feed()) pet->onUnlearnOffered(cost);
        }
        packet.skipAll();
    };
}

void PetHandler::beginCharacter() {
    pendingNameQueries_.clear();
    if (auto* pet = feed()) pet->beginCharacter();
}

// ============================================================
// The pet
// ============================================================

void PetHandler::handlePetSpells(network::Packet& packet) {
    const uint64_t guid = packet.hasRemaining(8) ? packet.readUInt64() : 0;
    if (guid == 0) {
        LOG_INFO("SMSG_PET_SPELLS: pet cleared");
        if (auto* pet = feed()) pet->onPetDismissed();
        return;
    }
    // Ask what it is called. Nothing else does, and the name a player gave a
    // pet comes back in answer to this and nothing else.
    requestName(guid);

    // Verified against AzerothCore's Player::PetSpellInitialize, which writes
    //   guid(8) family(2) duration(4) react(1) command(1) flags(2)
    // and then one uint32 per action-bar slot, a uint8 spell count, and one
    // packed uint32 per spell.
    //
    // This read the duration as a uint16 and never read the flags, so the
    // action bar started four bytes early: every slot held the one before it,
    // the first held the react and command bytes, and the last was lost. The
    // react and command themselves came out of the duration's upper half -
    // zero for a permanent pet, which is why they looked plausible.
    //
    // The family field is what a WotLK server sends for pet talents. Before
    // that the packet is the same one without it. What the packet ends before
    // is left as it was.
    pet::PetSpellsRecord record;
    record.guid = guid;
    do {
        if (!isPreWotlk()) {
            if (!packet.hasRemaining(2)) break;
            packet.readUInt16();  // creature family
        }
        if (!packet.hasRemaining(4)) break;
        packet.readUInt32();      // duration in ms; zero for a permanent pet

        if (!packet.hasRemaining(4)) break;
        pet::PetMode mode;
        mode.react   = packet.readUInt8();
        mode.command = packet.readUInt8();
        record.mode = mode;
        packet.readUInt16();      // flags, unused by the server too

        if (!packet.hasRemaining(pet::kActionBarSlots * 4u)) break;
        std::array<uint32_t, pet::kActionBarSlots> bar{};
        for (auto& slot : bar) slot = packet.readUInt32();
        record.actionBar = bar;

        if (!packet.hasRemaining(1)) break;
        const uint8_t spellCount = packet.readUInt8();
        auto& spells = record.spells.emplace();
        for (uint8_t i = 0; i < spellCount; ++i) {
            // One uint32, not six bytes: MAKE_UNIT_ACTION_BUTTON puts the spell
            // in the low twenty-four bits and the state in the top byte, the
            // same packing the action-bar slots above use and that
            // GetPetActionInfo already masks for. Read as four plus two, the
            // spell id carried the state and the autocast flag was two bytes of
            // the next spell - and every spell after the first was misaligned.
            if (!packet.hasRemaining(4)) break;
            const uint32_t packed  = packet.readUInt32();
            const uint32_t spellId = packed & 0x00FFFFFFu;
            const uint8_t  state   = static_cast<uint8_t>(packed >> 24);
            // ACT_ENABLED is 0xC1 and ACT_DISABLED 0x81 - 0x40 is the autocast
            // bit. The old test was & 0x0001, which is ACT_PASSIVE's bit.
            spells.push_back({.spellId = spellId, .autocast = (state & 0x40) != 0});
        }
    } while (false);

    LOG_INFO("SMSG_PET_SPELLS: petGuid=0x", std::hex, guid, std::dec,
             " react=", record.mode ? static_cast<int>(record.mode->react) : -1,
             " command=", record.mode ? static_cast<int>(record.mode->command) : -1,
             " spells=", record.spells ? record.spells->size() : 0);
    if (auto* pet = feed()) pet->onPetSpells(record);
}

void PetHandler::requestName(uint64_t petGuid) {
    if (petGuid == 0 || !sender_.characterInWorld()) return;
    // The pet number in the request is a key the server echoes back and does
    // not look anything up with: SendPetNameQuery finds the pet by guid. So
    // this sends a number of its own making and matches the reply with it.
    const uint32_t key = nextNameQueryKey_++;
    pendingNameQueries_[key] = petGuid;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_NAME_QUERY));
    pkt.writeUInt32(key);
    pkt.writeUInt64(petGuid);
    sender_.send(pkt);
}

void PetHandler::handlePetNameQueryResponse(network::Packet& packet) {
    // Shape from AzerothCore's SendPetNameQuery: the key back, the name, and
    // the timestamp the name was set. A pet it could not find answers a zero
    // byte where the name would be, which is the empty string and means "keep
    // what you have" rather than "the pet has no name".
    if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
    const uint32_t key = packet.readUInt32();
    const std::string name = packet.readString();
    packet.skipAll();

    auto it = pendingNameQueries_.find(key);
    if (it == pendingNameQueries_.end()) {
        LOG_WARNING("SMSG_PET_NAME_QUERY_RESPONSE: key ", key,
                    " answers no question this client asked - the name is dropped");
        return;
    }
    const uint64_t guid = it->second;
    pendingNameQueries_.erase(it);
    if (name.empty()) return;

    if (!queries_.nameUnitInView(guid, name)) {
        LOG_WARNING("Pet name for 0x", std::hex, guid, std::dec, " ('", name,
                    "') arrived after the pet left view; it keeps its template's name");
        return;
    }
    LOG_INFO("Pet name: 0x", std::hex, guid, std::dec, " is '", name, "'");
    // Both, because the two frames listen for different ones: the pet frame
    // redraws its name from UNIT_NAME_UPDATE, and the pet bar and anything
    // else keyed on the pet rebuild from PET_UI_UPDATE.
    if (guid == queries_.petGuid()) {
        frameXml_.fireFrameXmlEvent("UNIT_NAME_UPDATE", {"pet"});
        frameXml_.fireFrameXmlEvent("PET_UI_UPDATE");
    }
}

void PetHandler::handleTameFailure(network::Packet& packet) {
    static const char* reasons[] = {
        "Invalid creature", "Too many pets", "Already tamed",
        "Wrong faction", "Level too low", "Creature not tameable",
        "Can't control", "Can't command"
    };
    if (packet.hasRemaining(1)) {
        uint8_t reason = packet.readUInt8();
        const char* msg = (reason < 8) ? reasons[reason] : "Unknown reason";
        std::string s = std::string("Failed to tame: ") + msg;
        raiseUiError(s);
        chat_.addSystemChatMessage(s);
    }
}

void PetHandler::handleActionFeedback(network::Packet& packet) {
    static const char* kPetFeedback[] = {
        nullptr,
        "Your pet is dead.", "Your pet has nothing to attack.",
        "Your pet cannot attack that target.", "That target is too far away.",
        "Your pet cannot find a path to the target.",
        "Your pet cannot attack an immune target.",
    };
    if (!packet.hasRemaining(1)) return;
    uint8_t msg = packet.readUInt8();
    if (msg > 0 && msg < 7 && kPetFeedback[msg]) chat_.addSystemChatMessage(kPetFeedback[msg]);
    packet.skipAll();
}

void PetHandler::handleCastFailed(network::Packet& packet) {
    // WotLK: castCount(1) + spellId(4) + reason(1)
    // Classic/TBC: spellId(4) + reason(1) (no castCount)
    const bool hasCount = isActiveExpansion("wotlk");
    const size_t minSize = hasCount ? 6u : 5u;
    if (packet.hasRemaining(minSize)) {
        if (hasCount) /*uint8_t castCount =*/ packet.readUInt8();
        uint32_t spellId   = packet.readUInt32();
        uint8_t  reason    = (packet.hasRemaining(1)) ? packet.readUInt8() : 0;
        LOG_DEBUG("SMSG_PET_CAST_FAILED: spell=", spellId, " reason=", static_cast<int>(reason));
        if (reason != 0) {
            const char* reasonStr = getSpellCastResultString(reason);
            const std::string sName = queries_.spellName(spellId);
            std::string errMsg;
            if (reasonStr && *reasonStr)
                errMsg = sName.empty() ? reasonStr : (sName + ": " + reasonStr);
            else
                errMsg = sName.empty() ? "Pet spell failed." : (sName + ": Pet spell failed.");
            chat_.addSystemChatMessage(errMsg);
        }
    }
    packet.skipAll();
}

void PetHandler::handleMode(network::Packet& packet) {
    // uint64 petGuid, uint32 mode
    // mode bits: low byte = command state, next byte = react state
    if (packet.hasRemaining(12)) {
        const uint64_t modeGuid = packet.readUInt64();
        const uint32_t mode     = packet.readUInt32();
        pet::PetMode m;
        m.command = static_cast<uint8_t>(mode & 0xFF);
        m.react   = static_cast<uint8_t>((mode >> 8) & 0xFF);
        LOG_DEBUG("SMSG_PET_MODE: command=", static_cast<int>(m.command), " react=", static_cast<int>(m.react));
        if (auto* pet = feed()) pet->onMode(modeGuid, m);
    }
    packet.skipAll();
}

// ============================================================
// The stable
// ============================================================

void PetHandler::handleListStabledPets(network::Packet& packet) {
    constexpr size_t kMinHeader = 8 + 1 + 1;
    if (!packet.hasRemaining(kMinHeader)) {
        LOG_WARNING("MSG_LIST_STABLED_PETS: packet too short (", packet.getSize(), ")");
        return;
    }
    const uint64_t masterGuid = packet.readUInt64();
    const uint8_t petCount = packet.readUInt8();
    const uint8_t numSlots = packet.readUInt8();

    std::vector<pet::StabledPet> pets;
    pets.reserve(petCount);
    for (uint8_t i = 0; i < petCount; ++i) {
        // petNumber(4) + entry(4) + level(4) = 12 bytes before the name string
        if (!packet.hasRemaining(12)) break;
        pet::StabledPet pet;
        pet.petNumber = packet.readUInt32();
        pet.entry     = packet.readUInt32();
        pet.level     = packet.readUInt32();
        pet.name      = packet.readString();
        // One byte after the name, and one only: SendStablePet writes
        // uint32 PetNumber, uint32 CreatureId, uint32 Level, the name, then
        // `uint8(1)` for the pet that is out and `uint8(2)` for one in a stable
        // slot. There is no display id on the wire.
        if (!packet.hasRemaining(1)) break;
        pet.isActive  = (packet.readUInt8() == 1);
        pets.push_back(std::move(pet));
    }

    LOG_INFO("MSG_LIST_STABLED_PETS: stableMasterGuid=0x", std::hex, masterGuid, std::dec,
             " petCount=", static_cast<int>(petCount), " numSlots=", static_cast<int>(numSlots));
    for (const auto& p : pets) {
        LOG_DEBUG("  Pet: number=", p.petNumber, " entry=", p.entry,
                  " level=", p.level, " name='", p.name, "' active=", p.isActive);
    }
    if (auto* stable = feed()) stable->onStableList(masterGuid, numSlots, std::move(pets));
}

} // namespace wowee::game
