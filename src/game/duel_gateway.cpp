#include "game/duel_gateway.hpp"

#include "duel/services/duel_service.hpp"
#include "game/chat_log.hpp"
#include "game/entity.hpp"
#include "game/frame_xml_events.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"

namespace wowee::game {

bool DuelGateway::online() const {
    return sender_.characterInWorld();
}

void DuelGateway::sendAccept(uint64_t arbiter) {
    sender_.send(DuelAcceptPacket::build(arbiter));
}

void DuelGateway::sendCancel(uint64_t arbiter) {
    sender_.send(DuelCancelPacket::build(arbiter));
}

void DuelGateway::sendPropose(uint64_t target) {
    sender_.send(DuelProposedPacket::build(target));
}

uint64_t DuelGateway::playerGuid() const {
    return world_.characterGuid();
}

std::string DuelGateway::name(uint64_t guid) const {
    auto entity = world_.entities().getEntity(guid);
    if (auto* unit = dynamic_cast<Unit*>(entity.get()); unit && !unit->getName().empty()) return unit->getName();
    return world_.playerName(guid);
}

std::vector<core::EventBus::Subscription>
subscribeDuelNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace duel;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    s.push_back(events.subscribe<DuelRequested>([line, addon](const DuelRequested& e) {
        line(e.challengerName + " challenges you to a duel!");
        addon("DUEL_REQUESTED", {e.challengerName});
    }));
    s.push_back(events.subscribe<DuelFinished>([line, addon](const DuelFinished& e) {
        if (!e.started) line("The duel was cancelled.");
        addon("DUEL_FINISHED");
    }));
    s.push_back(events.subscribe<DuelAccepted>([line](const auto&) { line("You accept the duel."); }));
    s.push_back(events.subscribe<DuelForfeited>([line](const auto&) { line("You have forfeited the duel."); }));
    s.push_back(events.subscribe<DuelProposed>([line](const auto&) { line("You have challenged your target to a duel."); }));
    s.push_back(events.subscribe<NoDuelTarget>([&events, &frameXml](const auto&) {
        raiseUiError(events, frameXml, "You must target a player to challenge to a duel.");
    }));
    s.push_back(events.subscribe<DuelWinner>([line](const DuelWinner& e) {
        line(e.fled ? e.loser + " has fled from the duel. " + e.winner + " wins!"
                    : e.winner + " has defeated " + e.loser + " in a duel!");
    }));
    // The interface runs the ten-second countdown to a forfeit off this pair;
    // the message alone says it once and never counts.
    s.push_back(events.subscribe<DuelOutOfBounds>([&events, &frameXml, addon](const auto&) {
        raiseUiError(events, frameXml, "You are out of the duel area!");
        addon("DUEL_OUTOFBOUNDS");
    }));
    // Stops that countdown, for a duellist who stepped back inside.
    s.push_back(events.subscribe<DuelInBounds>([addon](const auto&) { addon("DUEL_INBOUNDS"); }));
    return s;
}

} // namespace wowee::game
