// Chat, end to end: packets from the server into the real game, and what the
// interface is told and what the game sends back out.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/world_packet_data.hpp"

#include <string>

using wowee::game::ChatType;
using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;

namespace {

constexpr uint64_t kThrall = 0x20;
constexpr uint64_t kJaina = 0x21;

/// SMSG_MESSAGECHAT for a player's line, as 3.3.5a sends it: a guid and no
/// name, which the game has to find out for itself.
std::vector<uint8_t> playerLine(ChatType type, uint64_t sender, uint64_t receiver,
                                const std::string& text) {
    return Body()
        .u8(static_cast<uint8_t>(type))
        .u32(0)                                   // Universal
        .u64(sender)
        .u32(0)
        .u64(receiver)
        .u32(static_cast<uint32_t>(text.size() + 1))
        .cstr(text)
        .u8(0);                                   // chat tag
}

std::vector<uint8_t> nameResponse(uint64_t guid, const std::string& name) {
    return Body().packedGuid(guid).u8(0 /* found */).cstr(name).cstr("").u8(2).u8(0).u8(7);
}

/// Who a CMSG_MESSAGECHAT whisper went to, and what it said.
struct SentWhisper {
    std::string target;
    std::string text;
};

SentWhisper readWhisper(wowee::network::Packet& packet) {
    REQUIRE(packet.readUInt32() == static_cast<uint32_t>(ChatType::WHISPER));
    packet.readUInt32();  // language
    SentWhisper w;
    w.target = packet.readString();
    w.text = packet.readString();
    return w;
}

}  // namespace

TEST_CASE("a whisper from a stranger waits for the name, then reaches the interface and /r",
          "[chat][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_MESSAGECHAT,
              playerLine(ChatType::WHISPER, kThrall, h.player().guid, "lok'tar"));
    h.pump();

    // Nobody here knows that guid, so the game asks - and does not show the
    // line with no name on it.
    CHECK(h.countSent(Opcode::CMSG_NAME_QUERY) == 1);
    CHECK(h.addonEvents("CHAT_MSG_WHISPER").empty());

    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, nameResponse(kThrall, "Thrall"));
    h.pump();

    const auto whispers = h.addonEvents("CHAT_MSG_WHISPER");
    REQUIRE(whispers.size() == 1);
    CHECK(whispers[0].args.at(0) == "lok'tar");
    CHECK(whispers[0].args.at(1) == "Thrall");

    const auto& history = h.game().getChatHistory();
    REQUIRE_FALSE(history.empty());
    CHECK(history.back().message == "lok'tar");
    CHECK(history.back().senderName == "Thrall");

    // /r answers whoever whispered last - the name the line arrived with.
    CHECK(h.game().getLastWhisperSender() == "Thrall");
    h.clearRecords();
    h.game().sendChatMessage(ChatType::WHISPER, "for the horde", h.game().getLastWhisperSender());
    auto* sent = h.lastSent(Opcode::CMSG_MESSAGECHAT);
    REQUIRE(sent != nullptr);
    const SentWhisper reply = readWhisper(*sent);
    CHECK(reply.target == "Thrall");
    CHECK(reply.text == "for the horde");
}

TEST_CASE("while away, each whisperer is answered once and the player stays away",
          "[chat][harness]") {
    GameHarness h;
    h.enterWorld();

    // Names already known, so the lines go straight through.
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, nameResponse(kThrall, "Thrall"));
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, nameResponse(kJaina, "Jaina"));
    h.pump();

    h.game().toggleAfk("back soon");
    h.clearRecords();

    h.receive(Opcode::SMSG_MESSAGECHAT, playerLine(ChatType::WHISPER, kThrall, h.player().guid, "hey"));
    h.receive(Opcode::SMSG_MESSAGECHAT, playerLine(ChatType::WHISPER, kThrall, h.player().guid, "hey?"));
    h.receive(Opcode::SMSG_MESSAGECHAT, playerLine(ChatType::WHISPER, kJaina, h.player().guid, "hello"));
    h.pump();

    // The auto-reply is the machine answering for an absent player, not the
    // player coming back: it must not take them out of AFK, and the second
    // whisperer is owed the same answer as the first.
    CHECK(h.game().isAfk());
    REQUIRE(h.countSent(Opcode::CMSG_MESSAGECHAT) == 2);

    std::vector<SentWhisper> replies;
    for (auto& packet : h.server().sent()) {
        if (h.game().getOpcodeTable().fromWire(packet.getOpcode()) != Opcode::CMSG_MESSAGECHAT) continue;
        packet.setReadPos(0);
        replies.push_back(readWhisper(packet));
    }
    REQUIRE(replies.size() == 2);
    CHECK(replies[0].target == "Thrall");
    CHECK(replies[0].text == "<AFK> back soon");
    CHECK(replies[1].target == "Jaina");

    // Typing is coming back, though: autoClearAFK takes the player out of it.
    h.game().sendChatMessage(ChatType::SAY, "I'm back");
    CHECK_FALSE(h.game().isAfk());
}

TEST_CASE("the server's echo of the player's own line is not a second line", "[chat][harness]") {
    GameHarness h;
    h.enterWorld();

    h.game().sendChatMessage(ChatType::SAY, "hello world");
    const size_t afterOwn = h.game().getChatHistory().size();
    const size_t saysAfterOwn = h.addonEvents("CHAT_MSG_SAY").size();

    h.receive(Opcode::SMSG_MESSAGECHAT,
              playerLine(ChatType::SAY, h.player().guid, h.player().guid, "hello world"));
    h.pump();

    CHECK(h.game().getChatHistory().size() == afterOwn);
    CHECK(h.addonEvents("CHAT_MSG_SAY").size() == saysAfterOwn);
}
