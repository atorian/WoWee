#include "game/pet_gateway.hpp"

#include <string>

#include "foundation/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/pet_handler.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "pet/domain/events.hpp"

namespace wowee::game {

bool PetGateway::online() const {
    return sender_.characterInWorld();
}

bool PetGateway::connected() const {
    return sender_.connected();
}

void PetGateway::sendAction(uint64_t petGuid, uint32_t action, uint64_t targetGuid) {
    sender_.send(PetActionPacket::build(petGuid, action, targetGuid));
    LOG_DEBUG("sendPetAction: petGuid=0x", std::hex, petGuid, " action=0x", action, " target=0x", targetGuid,
              std::dec);
}

void PetGateway::sendAutocast(uint64_t petGuid, uint32_t spellId, bool on) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_SPELL_AUTOCAST));
    pkt.writeUInt64(petGuid);
    pkt.writeUInt32(spellId);
    pkt.writeUInt8(on ? 1 : 0);
    sender_.send(pkt);
    LOG_DEBUG("togglePetSpellAutocast: spellId=", spellId, " autocast=", on);
}

void PetGateway::sendRename(uint64_t petGuid, const std::string& name) {
    sender_.send(PetRenamePacket::build(petGuid, name, 0));
    LOG_INFO("Sent CMSG_PET_RENAME: petGuid=0x", std::hex, petGuid, std::dec, " name='", name, "'");
}

void PetGateway::sendAbandon(uint64_t petGuid) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_ABANDON));
    pkt.writeUInt64(petGuid);
    sender_.send(pkt);
    LOG_INFO("Sent CMSG_PET_ABANDON for pet=0x", std::hex, petGuid, std::dec);
}

void PetGateway::sendUnlearnTalents() {
    // Sent, and this server throws it away: CMSG_PET_UNLEARN_TALENTS is
    // Handle_NULL in AzerothCore's opcode table, and pet talents are only ever
    // reset there through the AT_LOGIN_RESET_PET_TALENTS flag at the next
    // login. Kept rather than dropped, because the request is correct and a
    // server that implements it would act on it.
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_UNLEARN_TALENTS));
    sender_.send(pkt);
    LOG_INFO("confirmPetUnlearn: sent CMSG_PET_UNLEARN_TALENTS");
}

void PetGateway::requestStableList(uint64_t masterGuid) {
    sender_.send(ListStabledPetsPacket::build(masterGuid));
    LOG_INFO("Sent MSG_LIST_STABLED_PETS to npc=0x", std::hex, masterGuid, std::dec);
}

void PetGateway::sendStable(uint64_t masterGuid, uint8_t slot) {
    sender_.send(StablePetPacket::build(masterGuid, slot));
    LOG_INFO("Sent CMSG_STABLE_PET: slot=", static_cast<int>(slot));
}

void PetGateway::sendUnstable(uint64_t masterGuid, uint32_t petNumber) {
    sender_.send(UnstablePetPacket::build(masterGuid, petNumber));
    LOG_INFO("Sent CMSG_UNSTABLE_PET: petNumber=", petNumber);
}

void PetGateway::sendBuyStableSlot(uint64_t masterGuid) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_BUY_STABLE_SLOT));
    pkt.writeUInt64(masterGuid);
    sender_.send(pkt);
    LOG_INFO("Sent CMSG_BUY_STABLE_SLOT to npc=0x", std::hex, masterGuid, std::dec);
}

std::vector<core::EventBus::Subscription>
subscribePetNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, PetQueries& names) {
    using namespace pet;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    // The pet frame and the pet action bar both rebuild from these; UNIT_PET
    // alone tells them the pet changed but not that its interface should be
    // redrawn.
    auto petChanged = [&frameXml] {
        frameXml.fireFrameXmlEvent("UNIT_PET", {"player"});
        frameXml.fireFrameXmlEvent("PET_UI_UPDATE");
    };

    s.push_back(events.subscribe<PetChanged>([&frameXml, petChanged](const PetChanged& e) {
        petChanged();
        if (e.barChanged) frameXml.fireFrameXmlEvent("PET_BAR_UPDATE");
    }));
    // The same pair a summon fires, because the same two things have to be
    // redrawn: losing the pet outright announced nothing, so a dead pet's frame
    // and its whole ability bar stayed on screen until the next login.
    s.push_back(events.subscribe<PetLost>([line, petChanged](const auto&) {
        line("Your pet has died.");
        petChanged();
    }));
    s.push_back(events.subscribe<PetLearnedSpell>([line, &frameXml, &names](const PetLearnedSpell& e) {
        const std::string name = names.spellName(e.spellId);
        line("Your pet has learned " + (name.empty() ? "a new ability." : name + "."));
        frameXml.fireFrameXmlEvent("PET_BAR_UPDATE");
    }));
    s.push_back(events.subscribe<PetUnlearnedSpell>([&frameXml](const auto&) {
        frameXml.fireFrameXmlEvent("PET_BAR_UPDATE");
    }));
    // The pet frame lights its attack indicator on the start and clears it on
    // the stop.
    s.push_back(events.subscribe<PetAttackStarted>([&frameXml](const auto&) {
        frameXml.fireFrameXmlEvent("PET_ATTACK_START");
    }));
    s.push_back(events.subscribe<PetAttackStopped>([&frameXml](const auto&) {
        frameXml.fireFrameXmlEvent("PET_ATTACK_STOP");
    }));
    // The pet frame's experience bar reads GetPetExperience and redraws on
    // this; without it the bar was filled once when the pet was summoned and
    // never moved again.
    s.push_back(events.subscribe<PetExperienceChanged>([&frameXml](const auto&) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent("UNIT_PET_EXPERIENCE", {"pet"});
    }));
    // The pet tab redraws its stat block on these, named for the pet.
    s.push_back(events.subscribe<PetStatsChanged>([&frameXml](const auto&) {
        if (!frameXml.frameXmlListening()) return;
        frameXml.fireFrameXmlEvent("UNIT_STATS", {"pet"});
        frameXml.fireFrameXmlEvent("UNIT_RESISTANCES", {"pet"});
    }));
    // The naming dialog opens on this.
    s.push_back(events.subscribe<PetRenameable>([&frameXml](const auto&) {
        frameXml.fireFrameXmlEvent("PET_RENAMEABLE");
    }));
    // The list both opens the stable window and is the only thing that
    // refreshes it, so it carries both events.
    s.push_back(events.subscribe<StableShown>([&frameXml](const auto&) {
        frameXml.fireFrameXmlEvent("PET_STABLE_SHOW");
        frameXml.fireFrameXmlEvent("PET_STABLE_UPDATE");
    }));
    // The stable window hides on this and nothing else.
    s.push_back(events.subscribe<StableClosed>([&frameXml](const auto&) {
        frameXml.fireFrameXmlEvent("PET_STABLE_CLOSED");
    }));
    s.push_back(events.subscribe<StableResult>([line, &events, &frameXml](const StableResult& e) {
        const char* msg = nullptr;
        switch (e.result) {
            case 0x01: msg = "Pet stored in stable."; break;
            case 0x06: msg = "Pet retrieved from stable."; break;
            case 0x07: msg = "Stable slot purchased."; break;
            case 0x08: msg = "Stable list updated."; break;
            case 0x09:
                msg = "Stable failed: not enough money or other error.";
                raiseUiError(events, frameXml, msg);
                break;
            default: break;
        }
        if (msg) line(msg);
        LOG_INFO("SMSG_STABLE_RESULT: result=", static_cast<int>(e.result));
    }));
    s.push_back(events.subscribe<NoPetToStable>([&events, &frameXml](const auto&) {
        raiseUiError(events, frameXml, "You do not have an active pet to stable.");
    }));
    // The line says what was asked rather than claiming it happened: this
    // server resets pet talents at the next login.
    s.push_back(events.subscribe<PetTalentResetRequested>([line](const auto&) {
        line("Pet talent reset requested. This server resets pet talents at the "
             "next login rather than on request.");
    }));
    return s;
}

} // namespace wowee::game
