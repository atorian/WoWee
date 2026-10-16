#include "game/pet_handler.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/game_utils.hpp"
#include "game/frame_xml_events.hpp"
#include "game/packet_sender.hpp"
#include "game/pet_action.hpp"
#include "game/spell_defines.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

#include <algorithm>

namespace wowee::game {

PetHandler::PetHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                       ChatLog& chat, PetQueries& queries)
    : sender_(sender), frameXml_(frameXml), events_(events), chat_(chat), queries_(queries) {}

void PetHandler::raiseUiError(const std::string& message) {
    game::raiseUiError(events_, frameXml_, message);
}

void PetHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_PET_SPELLS] = [this](network::Packet& p) { handlePetSpells(p); };
    table[Opcode::MSG_LIST_STABLED_PETS] = [this](network::Packet& p) {
        if (sender_.characterInWorld()) handleListStabledPets(p);
    };
    table[Opcode::SMSG_STABLE_RESULT] = [this](network::Packet& p) { handleStableResult(p); };
    table[Opcode::SMSG_PET_NAME_QUERY_RESPONSE] = [this](network::Packet& p) {
        handlePetNameQueryResponse(p);
    };
    table[Opcode::SMSG_PET_TAME_FAILURE] = [this](network::Packet& p) { handleTameFailure(p); };
    table[Opcode::SMSG_PET_ACTION_FEEDBACK] = [this](network::Packet& p) { handleActionFeedback(p); };
    table[Opcode::SMSG_PET_CAST_FAILED] = [this](network::Packet& p) { handleCastFailed(p); };
    table[Opcode::SMSG_PET_BROKEN] = [this](network::Packet& p) { handleBroken(p); };
    table[Opcode::SMSG_PET_LEARNED_SPELL] = [this](network::Packet& p) { handleLearnedSpell(p); };
    table[Opcode::SMSG_PET_UNLEARNED_SPELL] = [this](network::Packet& p) { handleUnlearnedSpell(p); };
    table[Opcode::SMSG_PET_MODE] = [this](network::Packet& p) { handleMode(p); };
    // Server signals that the pet can now be named (first tame)
    table[Opcode::SMSG_PET_RENAMEABLE] = [this](network::Packet& packet) {
        held_.renameable = true;
        // The naming dialog opens on this. The flag alone is read by this
        // client's own UI, so the interface's version never appeared.
        frameXml_.fireFrameXmlEvent("PET_RENAMEABLE");
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
            unlearnCost_ = packet.readUInt32();
            unlearnPending_ = true;
        }
        packet.skipAll();
    };
}

void PetHandler::beginCharacter() {
    held_ = Held{};
    stable_ = Stable{};
    unlearnPending_ = false;
    unlearnCost_ = 0;
    pendingNameQueries_.clear();
}

// ============================================================
// The pet
// ============================================================

void PetHandler::announcePetChanged() {
    frameXml_.fireFrameXmlEvent("UNIT_PET", {"player"});
    // The pet frame and the pet action bar both rebuild from this one;
    // UNIT_PET alone tells them the pet changed but not that its interface
    // should be redrawn.
    frameXml_.fireFrameXmlEvent("PET_UI_UPDATE");
}

void PetHandler::clearPet() {
    held_.guid = 0;
    held_.spells.clear();
    held_.autocast.clear();
    held_.actionSlots = {};
}

void PetHandler::handlePetSpells(network::Packet& packet) {
    const size_t remaining = packet.getRemainingSize();
    if (remaining < 8) {
        clearPet();
        LOG_INFO("SMSG_PET_SPELLS: pet cleared");
        announcePetChanged();
        return;
    }

    held_.guid = packet.readUInt64();
    if (held_.guid == 0) {
        clearPet();
        LOG_INFO("SMSG_PET_SPELLS: pet cleared (guid=0)");
        announcePetChanged();
        return;
    }
    // Ask what it is called. Nothing else does, and the name a player gave a
    // pet comes back in answer to this and nothing else.
    requestName(held_.guid);

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
    // that the packet is the same one without it.
    do {
        if (!isPreWotlk()) {
            if (!packet.hasRemaining(2)) break;
            packet.readUInt16();  // creature family
        }
        if (!packet.hasRemaining(4)) break;
        packet.readUInt32();      // duration in ms; zero for a permanent pet

        if (!packet.hasRemaining(4)) break;
        held_.react   = packet.readUInt8();
        held_.command = packet.readUInt8();
        packet.readUInt16();      // flags, unused by the server too

        if (!packet.hasRemaining(kActionBarSlots * 4u)) break;
        for (auto& slot : held_.actionSlots) slot = packet.readUInt32();

        if (!packet.hasRemaining(1)) break;
        uint8_t spellCount = packet.readUInt8();
        held_.spells.clear();
        held_.autocast.clear();
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
            if (spellId == 0) continue;
            held_.spells.push_back(spellId);
            // ACT_ENABLED is 0xC1 and ACT_DISABLED 0x81 - 0x40 is the autocast
            // bit. The old test was & 0x0001, which is ACT_PASSIVE's bit.
            if (state & 0x40) held_.autocast.insert(spellId);
        }
    } while (false);

    LOG_INFO("SMSG_PET_SPELLS: petGuid=0x", std::hex, held_.guid, std::dec,
             " react=", static_cast<int>(held_.react), " command=", static_cast<int>(held_.command),
             " spells=", held_.spells.size());
    announcePetChanged();
    frameXml_.fireFrameXmlEvent("PET_BAR_UPDATE");
}

void PetHandler::setStat(int index, int32_t value) {
    if (index < 0 || index >= static_cast<int>(held_.stats.size())) return;
    held_.stats[static_cast<size_t>(index)] = value;
}

void PetHandler::setResistance(int index, int32_t value) {
    if (index < 0 || index >= static_cast<int>(held_.resistances.size())) return;
    held_.resistances[static_cast<size_t>(index)] = value;
}

bool PetHandler::setExperience(uint32_t value) {
    if (held_.experience == value) return false;
    held_.experience = value;
    return true;
}

bool PetHandler::setNextLevelExperience(uint32_t value) {
    if (held_.nextLevelExperience == value) return false;
    held_.nextLevelExperience = value;
    return true;
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
    if (guid == held_.guid) {
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

void PetHandler::handleBroken(network::Packet& packet) {
    // Pet bond broken (died or forcibly dismissed) - clear pet state
    clearPet();
    chat_.addSystemChatMessage("Your pet has died.");
    // The same pair SMSG_PET_SPELLS fires, because the same two things have to
    // be redrawn - the pet frame and the pet action bar. Losing the pet
    // outright announced nothing while *learning one spell* announced
    // PET_BAR_UPDATE, so a dead pet's frame and its whole ability bar stayed on
    // screen until the next login.
    announcePetChanged();
    LOG_INFO("SMSG_PET_BROKEN: pet bond broken");
    packet.skipAll();
}

void PetHandler::handleLearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        held_.spells.push_back(spellId);
        const std::string sname = queries_.spellName(spellId);
        chat_.addSystemChatMessage("Your pet has learned " + (sname.empty() ? "a new ability." : sname + "."));
        LOG_DEBUG("SMSG_PET_LEARNED_SPELL: spellId=", spellId);
        frameXml_.fireFrameXmlEvent("PET_BAR_UPDATE");
    }
    packet.skipAll();
}

void PetHandler::handleUnlearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        held_.spells.erase(std::remove(held_.spells.begin(), held_.spells.end(), spellId),
                           held_.spells.end());
        held_.autocast.erase(spellId);
        LOG_DEBUG("SMSG_PET_UNLEARNED_SPELL: spellId=", spellId);
        // The bar drew the spell; it is not the pet's any more.
        frameXml_.fireFrameXmlEvent("PET_BAR_UPDATE");
    }
    packet.skipAll();
}

void PetHandler::handleMode(network::Packet& packet) {
    // uint64 petGuid, uint32 mode
    // mode bits: low byte = command state, next byte = react state
    if (packet.hasRemaining(12)) {
        uint64_t modeGuid = packet.readUInt64();
        uint32_t mode     = packet.readUInt32();
        if (modeGuid == held_.guid) {
            // COMMAND_ATTACK is 2. The pet frame lights its attack indicator on
            // the start and clears it on the stop, so only the crossing matters
            // - this packet arrives for every mode change, and firing on each
            // would relight an indicator that is already lit.
            constexpr uint8_t kCommandAttack = 2;
            const bool wasAttacking = held_.command == kCommandAttack;
            held_.command = static_cast<uint8_t>(mode & 0xFF);
            held_.react   = static_cast<uint8_t>((mode >> 8) & 0xFF);
            const bool nowAttacking = held_.command == kCommandAttack;
            if (nowAttacking != wasAttacking) {
                frameXml_.fireFrameXmlEvent(nowAttacking ? "PET_ATTACK_START" : "PET_ATTACK_STOP");
            }
            LOG_DEBUG("SMSG_PET_MODE: command=", static_cast<int>(held_.command),
                      " react=", static_cast<int>(held_.react));
        }
    }
    packet.skipAll();
}

// ============================================================
// Commanding it
// ============================================================

void PetHandler::sendAction(uint32_t action, uint64_t targetGuid) {
    if (!hasPet() || !sender_.characterInWorld()) return;
    sender_.send(PetActionPacket::build(held_.guid, action, targetGuid));
    LOG_DEBUG("sendPetAction: petGuid=0x", std::hex, held_.guid,
              " action=0x", action, " target=0x", targetGuid, std::dec);
}

void PetHandler::dismiss() {
    if (!hasPet() || !sender_.characterInWorld()) return;
    // Dismiss is COMMAND_ABANDON. Packing action 0 here sent COMMAND_STAY, so
    // the pet planted itself instead of leaving.
    sender_.send(PetActionPacket::build(
        held_.guid, pet::packPetAction(pet::ActionType::Command, pet::kAbandon)));
}

void PetHandler::toggleAutocast(uint32_t spellId) {
    if (!hasPet() || spellId == 0 || !sender_.characterInWorld()) return;
    const bool currentlyOn = held_.autocast.count(spellId) != 0;
    const uint8_t newState = currentlyOn ? 0 : 1;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_SPELL_AUTOCAST));
    pkt.writeUInt64(held_.guid);
    pkt.writeUInt32(spellId);
    pkt.writeUInt8(newState);
    sender_.send(pkt);
    if (newState) held_.autocast.insert(spellId);
    else          held_.autocast.erase(spellId);
    LOG_DEBUG("togglePetSpellAutocast: spellId=", spellId, " autocast=", static_cast<int>(newState));
}

void PetHandler::rename(const std::string& newName) {
    if (!hasPet() || !sender_.characterInWorld()) return;
    if (newName.empty() || newName.size() > 12) return;
    sender_.send(PetRenamePacket::build(held_.guid, newName, 0));
    LOG_INFO("Sent CMSG_PET_RENAME: petGuid=0x", std::hex, held_.guid, std::dec, " name='", newName, "'");
}

// Abandoning is not dismissing. Dismiss puts a hunter's pet away and it can be
// called back; abandon gives it up for good, which is why the interface asks
// first and why this is its own message rather than another pet action.
void PetHandler::abandon() {
    if (!hasPet() || !sender_.characterInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_ABANDON));
    pkt.writeUInt64(held_.guid);
    sender_.send(pkt);
    LOG_INFO("Sent CMSG_PET_ABANDON for pet=0x", std::hex, held_.guid, std::dec);
}

void PetHandler::confirmUnlearn() {
    if (!unlearnPending_) return;
    unlearnPending_ = false;
    if (!sender_.characterInWorld()) return;

    // Sent, and this server throws it away: CMSG_PET_UNLEARN_TALENTS is
    // Handle_NULL in AzerothCore's opcode table, and pet talents are only ever
    // reset there through the AT_LOGIN_RESET_PET_TALENTS flag at the next
    // login. Kept rather than dropped, because the request is correct and a
    // server that implements it would act on it - but the line says what was
    // asked rather than claiming it happened.
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_UNLEARN_TALENTS));
    sender_.send(pkt);
    LOG_INFO("confirmPetUnlearn: sent CMSG_PET_UNLEARN_TALENTS");
    chat_.addSystemChatMessage(
        "Pet talent reset requested. This server resets pet talents at the "
        "next login rather than on request.");
    unlearnCost_ = 0;
}

// ============================================================
// The stable
// ============================================================

void PetHandler::closeStableWindow() {
    const bool wasOpen = stable_.windowOpen;
    stable_.windowOpen = false;
    // The stable window hides on this and nothing else.
    if (wasOpen) frameXml_.fireFrameXmlEvent("PET_STABLE_CLOSED");
}

void PetHandler::setStableMaster(uint64_t npcGuid) {
    stable_.masterGuid = npcGuid;
    stable_.windowOpen = false;
}

void PetHandler::handleListStabledPets(network::Packet& packet) {
    constexpr size_t kMinHeader = 8 + 1 + 1;
    if (!packet.hasRemaining(kMinHeader)) {
        LOG_WARNING("MSG_LIST_STABLED_PETS: packet too short (", packet.getSize(), ")");
        return;
    }
    stable_.masterGuid = packet.readUInt64();
    uint8_t petCount  = packet.readUInt8();
    stable_.numSlots  = packet.readUInt8();

    stable_.pets.clear();
    stable_.pets.reserve(petCount);
    for (uint8_t i = 0; i < petCount; ++i) {
        // petNumber(4) + entry(4) + level(4) = 12 bytes before the name string
        if (!packet.hasRemaining(12)) break;
        StabledPet pet;
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
        stable_.pets.push_back(std::move(pet));
    }

    stable_.windowOpen = true;
    LOG_INFO("MSG_LIST_STABLED_PETS: stableMasterGuid=0x", std::hex, stable_.masterGuid, std::dec,
             " petCount=", static_cast<int>(petCount), " numSlots=", static_cast<int>(stable_.numSlots));
    for (const auto& p : stable_.pets) {
        LOG_DEBUG("  Pet: number=", p.petNumber, " entry=", p.entry,
                  " level=", p.level, " name='", p.name, "' active=", p.isActive);
    }
    // This packet both opens the stable window and is the only thing that
    // refreshes it, so it carries both events.
    frameXml_.fireFrameXmlEvent("PET_STABLE_SHOW");
    frameXml_.fireFrameXmlEvent("PET_STABLE_UPDATE");
}

void PetHandler::handleStableResult(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    uint8_t result = packet.readUInt8();
    const char* msg = nullptr;
    switch (result) {
        case 0x01: msg = "Pet stored in stable."; break;
        case 0x06: msg = "Pet retrieved from stable."; break;
        case 0x07: msg = "Stable slot purchased."; break;
        case 0x08: msg = "Stable list updated."; break;
        case 0x09: msg = "Stable failed: not enough money or other error."; raiseUiError(msg); break;
        default: break;
    }
    if (msg) chat_.addSystemChatMessage(msg);
    LOG_INFO("SMSG_STABLE_RESULT: result=", static_cast<int>(result));
    if (stable_.windowOpen && stable_.masterGuid != 0 && sender_.connected() && result <= 0x08) {
        sender_.send(ListStabledPetsPacket::build(stable_.masterGuid));
    }
}

void PetHandler::requestStabledPetList() {
    if (!sender_.characterInWorld() || stable_.masterGuid == 0) return;
    sender_.send(ListStabledPetsPacket::build(stable_.masterGuid));
    LOG_INFO("Sent MSG_LIST_STABLED_PETS to npc=0x", std::hex, stable_.masterGuid, std::dec);
}

void PetHandler::stablePet(uint8_t slot) {
    if (!sender_.characterInWorld() || stable_.masterGuid == 0) return;
    if (!hasPet()) {
        raiseUiError("You do not have an active pet to stable.");
        return;
    }
    sender_.send(StablePetPacket::build(stable_.masterGuid, slot));
    LOG_INFO("Sent CMSG_STABLE_PET: slot=", static_cast<int>(slot));
}

// The stable master sells a fourth, fifth and sixth slot; the panel's buy
// button asks for one at a time and the server bills for whichever is next.
// CMSG_BUY_STABLE_SLOT carries nothing but who is being asked.
void PetHandler::buyStableSlot() {
    if (!sender_.characterInWorld() || stable_.masterGuid == 0) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_BUY_STABLE_SLOT));
    pkt.writeUInt64(stable_.masterGuid);
    sender_.send(pkt);
    LOG_INFO("Sent CMSG_BUY_STABLE_SLOT to npc=0x", std::hex, stable_.masterGuid, std::dec);
}

void PetHandler::unstablePet(uint32_t petNumber) {
    if (!sender_.characterInWorld() || stable_.masterGuid == 0 || petNumber == 0) return;
    sender_.send(UnstablePetPacket::build(stable_.masterGuid, petNumber));
    LOG_INFO("Sent CMSG_UNSTABLE_PET: petNumber=", petNumber);
}

} // namespace wowee::game
