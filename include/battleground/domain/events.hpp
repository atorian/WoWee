#pragma once

#include "battleground/domain/records.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::battleground {

// What happened to the player's battlegrounds. Battlegrounds returns the
// first five; the service publishes them and the server's answers, and the
// chat log and the addon layer subscribe.

/// A queue slot's state moved, worth a line.
struct QueueStatusChanged {
    uint32_t slot = 0;
    uint32_t statusId = 0;
    uint32_t bgTypeId = 0;
    std::string bgName;
    uint32_t avgWaitSec = 0;
    uint32_t waitedSec = 0;
};
/// A queue changed (UPDATE_BATTLEFIELD_STATUS); `slot` counts from one, which
/// is how battlefieldframe.lua knows which queue's prompt to raise.
struct QueueSlotUpdated { uint32_t slot = 0; };
/// A queue left or an invitation turned down.
struct QueueDeclined { bool wasInvite = false; std::string bgName; };
/// The scoreboard arrived (UPDATE_BATTLEFIELD_SCORE).
struct ScoreUpdated {};
/// A join the server let pass without a word: a realm that keeps a
/// battleground back drops the request unanswered.
struct JoinUnanswered { uint32_t bgTypeId = 0; };

using BattlegroundChange = std::variant<QueueStatusChanged, QueueSlotUpdated, QueueDeclined, ScoreUpdated,
                                        JoinUnanswered>;

/// An invitation answered, or a queue left, with none to answer.
struct NoInvitation {};
struct EnteringBattleground {};
/// The group joined a queue; `bgTypeId` names which.
struct GroupQueued { uint32_t bgTypeId = 0; };
/// The server refused a queue. `member` names who could not join, for
/// MemberUnavailable; `code` is the server's number, for Other.
struct JoinRefused { JoinRefusal reason = JoinRefusal::Other; int32_t code = 0; std::string member; };
struct PortDenied {};
struct RemovedFromQueue {};
struct JoinedQueue {};
struct PlayerEntered { std::string name; };
struct PlayerLeft { std::string name; };
struct AfkReportAnswered { bool accepted = false; };

} // namespace wowee::battleground
