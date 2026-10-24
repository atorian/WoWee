#include "game/petition_gateway.hpp"

#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/opcode_table.hpp"
#include "game/packet_sender.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packet_data.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "petition/domain/events.hpp"

namespace wowee::game {

using petition::OfferRefusal;
using petition::SignRefusal;
using petition::TurnInRefusal;

bool PetitionGateway::online() const {
    return sender_.characterInWorld();
}

bool PetitionGateway::connected() const {
    return sender_.connected();
}

void PetitionGateway::sendBuy(uint64_t npcGuid, const std::string& name, uint32_t clientIndex) {
    sender_.send(PetitionBuyPacket::build(npcGuid, name, clientIndex));
}

void PetitionGateway::sendSign(uint64_t petitionGuid) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_PETITION_SIGN));
    pkt.writeUInt64(petitionGuid);
    pkt.writeUInt8(0);
    sender_.send(pkt);
}

void PetitionGateway::sendTurnIn(uint64_t petitionGuid) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_TURN_IN_PETITION));
    pkt.writeUInt64(petitionGuid);
    sender_.send(pkt);
}

void PetitionGateway::sendOffer(uint64_t petitionGuid, uint64_t targetGuid) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_OFFER_PETITION));
    // The leading dword is not the petition type despite where it sits - the
    // server reads it and throws it away. Every expansion this client speaks
    // has it, so it is written unconditionally rather than gated.
    pkt.writeUInt32(0);
    pkt.writeUInt64(petitionGuid);
    pkt.writeUInt64(targetGuid);
    sender_.send(pkt);
}

void PetitionGateway::sendRename(uint64_t petitionGuid, const std::string& name) {
    // MSG_, not CMSG_: the server answers on the same opcode with the name it
    // accepted, which is why the interface does not redraw from its own text.
    network::Packet packet(wireOpcode(Opcode::MSG_PETITION_RENAME));
    packet.writeUInt64(petitionGuid);
    packet.writeString(name);
    sender_.send(packet);
}

void PetitionGateway::sendQuery(uint32_t entry, uint64_t petitionGuid) {
    sender_.send(PetitionQueryPacket::build(entry, petitionGuid));
}

petition::CharterOffer toCharterOffer(const PetitionShowlistData& data) {
    petition::CharterOffer offer;
    offer.npcGuid = data.npcGuid;
    offer.cost = data.cost;
    // Two panels, one opcode: the guild registrar and the arena registrar are
    // told apart by which charter is on offer, not by what was sent.
    offer.guildCharter = data.isGuildCharter();
    // The whole offer, not just the first charter's price. An arena registrar
    // offers three and the panel prices whichever tab is open.
    for (const auto& c : data.charters) offer.charters.push_back({c.itemId, c.displayId, c.cost});
    return offer;
}

SignRefusal signRefusal(uint32_t result) {
    switch (result) {
        case 1: return SignRefusal::AlreadySigned;
        case 2: return SignRefusal::AlreadyInGuild;
        case 3: return SignRefusal::OwnCharter;
        default: return SignRefusal::Other;
    }
}

TurnInRefusal turnInRefusal(uint32_t result) {
    switch (result) {
        case 1: return TurnInRefusal::AlreadyInGuild;
        case 2: return TurnInRefusal::NotEnoughSignatures;
        case 3: return TurnInRefusal::NameTaken;
        default: return TurnInRefusal::Other;
    }
}

OfferRefusal offerRefusal(uint32_t error) {
    switch (error) {
        case 1: return OfferRefusal::TargetInGuild;
        case 2: return OfferRefusal::TargetHasCharter;
        default: return OfferRefusal::Other;
    }
}

std::vector<core::EventBus::Subscription>
subscribePetitionNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace petition;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name);
    };
    auto error = [&events, &frameXml](const char* text) { raiseUiError(events, frameXml, text); };

    // The vendor panel opens on this, and closing has to name the same one.
    s.push_back(events.subscribe<VendorShown>([addon](const VendorShown& e) {
        addon(e.guild ? "GUILD_REGISTRAR_SHOW" : "PETITION_VENDOR_SHOW");
    }));
    s.push_back(events.subscribe<VendorClosed>([addon](const VendorClosed& e) {
        addon(e.guild ? "GUILD_REGISTRAR_CLOSED" : "PETITION_VENDOR_CLOSED");
    }));
    // The signature list frame opens on this, and redraws on it once the
    // query reply has said what the charter is.
    s.push_back(events.subscribe<PetitionShown>([addon](const auto&) { addon("PETITION_SHOW"); }));

    s.push_back(events.subscribe<PetitionSigned>([line](const auto&) { line("Petition signed successfully."); }));
    s.push_back(events.subscribe<SignRefused>([line, error](const SignRefused& e) {
        switch (e.reason) {
            case SignRefusal::AlreadySigned: error("You have already signed that petition."); break;
            case SignRefusal::AlreadyInGuild: error("You are already in a guild."); break;
            case SignRefusal::OwnCharter: error("You cannot sign your own petition."); break;
            case SignRefusal::Other: line("Cannot sign petition (error " + std::to_string(e.code) + ")."); break;
        }
    }));
    s.push_back(events.subscribe<CharterTurnedIn>([line](const auto&) { line("Guild created successfully!"); }));
    s.push_back(events.subscribe<TurnInRefused>([line](const TurnInRefused& e) {
        switch (e.reason) {
            case TurnInRefusal::AlreadyInGuild: line("Guild creation failed: already in a guild."); break;
            case TurnInRefusal::NotEnoughSignatures: line("Guild creation failed: not enough signatures."); break;
            case TurnInRefusal::NameTaken: line("Guild creation failed: name already taken."); break;
            case TurnInRefusal::Other: line("Guild creation failed (error " + std::to_string(e.code) + ")."); break;
        }
    }));
    s.push_back(events.subscribe<OfferRefused>([line, error](const OfferRefused& e) {
        switch (e.reason) {
            case OfferRefusal::TargetInGuild: line("Player is already in a guild."); break;
            case OfferRefusal::TargetHasCharter: line("Player already has a petition."); break;
            case OfferRefusal::Other: error("Cannot offer petition to that player."); break;
        }
    }));
    return s;
}

} // namespace wowee::game
