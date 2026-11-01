// The dungeon finder, end to end: the server's LFG packets, what the
// interface reads and is told, the lines and refusals the player sees, and
// the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "lfg/services/lfg.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::lfg::LfgState;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint32_t kDungeon = 261;       // an LFGDungeons id
constexpr uint64_t kVictim = 0x21;

/// SMSG_LFG_QUEUE_STATUS: the dungeon, five waits in seconds, what each role
/// still needs, and how long the player has queued.
std::vector<uint8_t> queueStatus(uint32_t queuedSeconds) {
    return Body().u32(kDungeon).u32(300).u32(240).u32(60).u32(120).u32(600)
        .u8(1).u8(0).u8(2).u32(queuedSeconds).bytes();
}

/// SMSG_LFG_PROPOSAL_UPDATE with two members, the player first.
std::vector<uint8_t> proposal(uint8_t state, uint32_t proposalId) {
    Body b;
    b.u32(kDungeon | (1u << 24)).u8(state).u32(proposalId).u32(0).u8(0 /* not silent */).u8(2);
    b.u32(0x02).u8(1).u8(0).u8(0).u8(1).u8(1);    // the player, a tank, accepted
    b.u32(0x08).u8(0).u8(0).u8(0).u8(0).u8(0);    // someone yet to answer
    return b.bytes();
}

/// SMSG_LFG_ROLE_CHECK_UPDATE for one dungeon and a group of three.
std::vector<uint8_t> roleCheck(uint32_t state) {
    return Body().u32(state).u8(state == 2 ? 1 : 0).u8(1).u32(kDungeon | (1u << 24)).u8(3).bytes();
}

/// SMSG_LFG_UPDATE_PLAYER, with or without its body.
std::vector<uint8_t> playerUpdate(uint8_t updateType, bool body, bool queued) {
    Body b;
    b.u8(updateType).u8(body ? 1 : 0);
    if (body) b.u8(queued ? 1 : 0).u8(0).u8(0).u8(1).u32(kDungeon).cstr("");
    return b.bytes();
}

/// SMSG_LFG_PLAYER_INFO with no random dungeons and one lock.
std::vector<uint8_t> playerInfo(uint32_t lockedDungeon, uint32_t reason) {
    return Body().u8(0).u32(1).u32(lockedDungeon | (1u << 24)).u32(reason).bytes();
}

/// SMSG_LFG_BOOT_PROPOSAL_UPDATE.
std::vector<uint8_t> bootVote(bool inProgress, uint32_t votes, uint32_t needed) {
    return Body().u8(inProgress ? 1 : 0).u8(0).u8(0).u64(kVictim)
        .u32(5).u32(votes).u32(30).u32(needed).cstr("afk").bytes();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

size_t chatCount(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return static_cast<size_t>(std::count_if(history.begin(), history.end(),
                                             [&](const auto& line) { return line.message == text; }));
}

std::vector<std::string> uiErrors(GameHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.addonEvents("UI_ERROR_MESSAGE")) out.push_back(e.args.at(0));
    return out;
}

}  // namespace

TEST_CASE("a join the server takes queues the player; a refusal says why and keeps them", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LFG_JOIN_RESULT, Body().u32(0).u32(2 /* queued */).bytes());
    h.pump();
    CHECK(h.lfg().isLfgQueued());
    CHECK(chatSaid(h, "Dungeon Finder: Joined the queue."));
    CHECK(h.addonEvents("LFG_UPDATE").size() == 1);

    h.receive(Opcode::SMSG_LFG_JOIN_RESULT, Body().u32(11).u32(0).bytes());
    h.pump();
    CHECK(uiErrors(h) == std::vector<std::string>{"Dungeon Finder: You are marked as a deserter."});
    CHECK(h.lfg().isLfgQueued());
    CHECK(h.addonEvents("LFG_UPDATE").size() == 2);
}

TEST_CASE("the queue's numbers are what the panel reads, and the minimap is told", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LFG_QUEUE_STATUS, queueStatus(95));
    h.pump();

    CHECK(h.lfg().getLfgState() == LfgState::Queued);
    CHECK(h.lfg().getLfgDungeonId() == kDungeon);
    CHECK(h.lfg().getLfgAvgWaitSec() == 300);
    CHECK(h.lfg().getLfgMyWaitSec() == 240);
    CHECK(h.lfg().getLfgNeedDps() == 2);
    CHECK(h.lfg().getLfgQueuedSeconds() == 95);
    CHECK(h.addonEvents("LFG_QUEUE_STATUS_UPDATE").size() == 1);
    CHECK(h.addonEvents("LFG_UPDATE").size() == 1);
}

TEST_CASE("a proposal opens its dialog once and ends in the dungeon", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LFG_PROPOSAL_UPDATE, proposal(0, 7));
    h.receive(Opcode::SMSG_LFG_PROPOSAL_UPDATE, proposal(0, 7));   // restated as someone answers
    h.pump();

    CHECK(h.lfg().getLfgState() == LfgState::Proposal);
    CHECK(h.lfg().getLfgProposalId() == 7);
    REQUIRE(h.lfg().getLfgProposalMembers().size() == 2);
    CHECK(h.lfg().getLfgProposalMembers()[0].isSelf);
    CHECK(h.addonEvents("LFG_PROPOSAL_SHOW").size() == 1);
    CHECK(h.addonEvents("LFG_PROPOSAL_UPDATE").size() == 2);
    CHECK(chatCount(h, "Dungeon Finder: A group has been found. Accept or decline.") == 2);

    h.lfg().lfgAcceptProposal(h.lfg().getLfgProposalId(), true);
    auto* answer = h.lastSent(Opcode::CMSG_LFG_PROPOSAL_RESULT);
    REQUIRE(answer);
    CHECK(answer->readUInt32() == 7);
    CHECK(answer->readUInt8() == 1);

    h.receive(Opcode::SMSG_LFG_PROPOSAL_UPDATE, proposal(2, 7));
    h.pump();
    CHECK(h.lfg().isLfgInDungeon());
    CHECK(h.lfg().getLfgProposalId() == 0);
    CHECK(h.addonEvents("LFG_PROPOSAL_SUCCEEDED").size() == 1);
    CHECK(chatSaid(h, "Dungeon Finder: Group found! Entering dungeon..."));
}

TEST_CASE("a role check's popup shows as it begins and a missing role refuses it", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LFG_ROLE_CHECK_UPDATE, roleCheck(2));
    h.pump();
    CHECK(h.lfg().getLfgState() == LfgState::RoleCheck);
    CHECK(h.lfg().getLfgRoleCheckMembers() == 3);
    REQUIRE(h.lfg().getLfgRoleCheckDungeons().size() == 1);
    CHECK(h.lfg().getLfgRoleCheckDungeons()[0].dungeonId == kDungeon);
    CHECK(h.addonEvents("LFG_ROLE_CHECK_SHOW").size() == 1);
    CHECK(chatSaid(h, "Dungeon Finder: Performing role check..."));

    h.receive(Opcode::SMSG_LFG_ROLE_CHECK_UPDATE, roleCheck(3));
    h.pump();
    CHECK(h.lfg().getLfgState() == LfgState::None);
    CHECK(h.addonEvents("LFG_ROLE_CHECK_HIDE").size() == 1);
    CHECK(uiErrors(h) == std::vector<std::string>{"Dungeon Finder: Role check failed - missing required role."});
}

TEST_CASE("a queue update takes its state from its flags and speaks only on a change", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_LFG_QUEUE_STATUS, queueStatus(10));
    h.pump();

    // Fifteen is the server restating the queue, with the queue flag set.
    h.receive(Opcode::SMSG_LFG_UPDATE_PLAYER, playerUpdate(15, true, true));
    h.receive(Opcode::SMSG_LFG_UPDATE_PLAYER, playerUpdate(13, true, true));   // added, but already queued
    h.pump();
    CHECK(h.lfg().isLfgQueued());
    CHECK_FALSE(chatSaid(h, "Dungeon Finder: Added to the queue."));

    h.receive(Opcode::SMSG_LFG_UPDATE_PLAYER, playerUpdate(8, false, false));  // removed, no body
    h.pump();
    CHECK(h.lfg().getLfgState() == LfgState::None);
    CHECK(chatSaid(h, "Dungeon Finder: Removed from the queue."));
}

TEST_CASE("a party update with the join flag and no queue flag is the dungeon", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    // SMSG_LFG_UPDATE_PARTY: the type, a body, joined, not queued, five bytes,
    // then the dungeons and the comment.
    h.receive(Opcode::SMSG_LFG_UPDATE_PARTY,
              Body().u8(15).u8(1).u8(1).u8(0).u8(0).u8(0).u8(0).u8(0).u8(0)
                  .u8(1).u32(kDungeon).cstr("").bytes());
    h.pump();

    CHECK(h.lfg().isLfgInDungeon());
    CHECK(h.lfg().getLfgDungeonId() == kDungeon);
}

TEST_CASE("the locks are what the panel reads, and a cut-short list leaves them", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LFG_PLAYER_INFO, playerInfo(43, 2));
    h.pump();
    REQUIRE(h.lfg().getLfgLocks().count(43) == 1);
    CHECK(h.lfg().getLfgLocks().at(43) == 2);
    CHECK(h.addonEvents("LFG_LOCK_INFO_RECEIVED").size() == 1);
    CHECK(h.addonEvents("LFG_UPDATE_RANDOM_INFO").size() == 1);

    // One random dungeon announced and not there.
    h.receive(Opcode::SMSG_LFG_PLAYER_INFO, Body().u8(1).bytes());
    h.pump();
    CHECK(h.lfg().getLfgLocks().count(43) == 1);
    CHECK(h.addonEvents("LFG_LOCK_INFO_RECEIVED").size() == 1);
}

TEST_CASE("a finished run's reward is kept for the toast to read back", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LFG_PLAYER_REWARD,
              Body().u32(258).u32(kDungeon).u8(1).u32(1).u32(25000).u32(5400).u32(0).u32(0)
                  .u8(1).u32(49426).u32(0).u32(2).bytes());
    h.pump();

    const auto& reward = h.lfg().getLfgCompletionReward();
    CHECK(reward.valid);
    CHECK(reward.money == 25000);
    CHECK(reward.xp == 5400);
    REQUIRE(reward.items.size() == 1);
    CHECK(reward.items[0].count == 2);
    CHECK(h.lfg().getLfgState() == LfgState::FinishedDungeon);
    CHECK(h.addonEvents("LFG_COMPLETION_REWARD").size() == 1);
}

TEST_CASE("a vote to kick names its victim, and its end is said", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE,
              Body().packedGuid(kVictim).u8(0).cstr("Jaina").cstr("").u8(1).u8(1).u8(8).bytes());
    h.pump();

    h.receive(Opcode::SMSG_LFG_BOOT_PROPOSAL_UPDATE, bootVote(true, 2, 3));
    h.pump();
    CHECK(h.lfg().isLfgBootInProgress());
    CHECK(h.lfg().getLfgBootTargetName() == "Jaina");
    CHECK(h.lfg().getLfgBootReason() == "afk");
    CHECK(h.lfg().getLfgBootVotes() == 2);

    h.receive(Opcode::SMSG_LFG_BOOT_PROPOSAL_UPDATE, bootVote(false, 3, 3));
    h.pump();
    CHECK_FALSE(h.lfg().isLfgBootInProgress());
    CHECK(chatSaid(h, "Dungeon Finder: Vote kick passed - member removed."));
    CHECK(h.addonEvents("LFG_BOOT_PROPOSAL_UPDATE").size() == 2);
}

TEST_CASE("a refused teleport says why", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_LFG_TELEPORT_DENIED, Body().u32(3).bytes());
    h.pump();

    CHECK(chatSaid(h, "Dungeon Finder: You are dead."));
}

TEST_CASE("joining, leaving and offering roles go to the server", "[lfg][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_LFG_QUEUE_STATUS, queueStatus(10));
    h.pump();

    h.lfg().lfgSetRoles(0x02 | 0x08);
    h.pump();
    CHECK(h.lfg().getLfgOfferedRoles() == (0x02 | 0x08));
    CHECK(h.addonEvents("LFG_ROLE_UPDATE").size() == 1);
    auto* roles = h.lastSent(Opcode::CMSG_LFG_SET_ROLES);
    REQUIRE(roles);
    CHECK(roles->readUInt8() == (0x02 | 0x08));

    h.lfg().lfgJoin({kDungeon}, 0x02);
    CHECK(h.lastSent(Opcode::CMSG_LFG_JOIN));

    h.lfg().lfgLeave();
    CHECK(h.lastSent(Opcode::CMSG_LFG_LEAVE));
    CHECK(h.lfg().getLfgState() == LfgState::None);
}

TEST_CASE("the next character has none of the last one's dungeon finder", "[lfg][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_LFG_QUEUE_STATUS, queueStatus(10));
    h.receive(Opcode::SMSG_LFG_PLAYER_INFO, playerInfo(43, 2));
    h.pump();
    h.lfg().lfgSetRoles(0x04);
    REQUIRE(h.lfg().isLfgQueued());

    h.relogAs(bob);

    CHECK(h.lfg().getLfgState() == LfgState::None);
    CHECK(h.lfg().getLfgLocks().empty());
    CHECK(h.lfg().getLfgOfferedRoles() == 0);
    CHECK(h.lfg().getLfgQueuedSeconds() == 0);
}
