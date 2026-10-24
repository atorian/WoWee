#pragma once

#include "lfg/domain/records.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::lfg {

// What happened in the dungeon finder. DungeonFinder returns the LfgChange
// ones; the service publishes them and the server's notices, and the chat
// log and the addon layer subscribe.

/// The queue state moved; GetLFGInfoServer answers the new one (LFG_UPDATE).
struct QueueUpdated {};
struct JoinedQueue { uint32_t dungeonId = 0; };
struct JoinFailed { JoinRefusal reason = JoinRefusal::Other; };
/// Locks and random rewards arrived (LFG_LOCK_INFO_RECEIVED,
/// LFG_UPDATE_RANDOM_INFO).
struct LockInfoReceived {};
struct QueueStatusUpdated {};
struct ProposalOffered { uint32_t dungeonId = 0; };
struct ProposalFailed {};
struct ProposalSucceeded { uint32_t dungeonId = 0; };
/// The proposal dialog re-reads GetLFGProposal (LFG_PROPOSAL_UPDATE).
struct ProposalUpdated {};
/// The dialog opens, once per proposal: the server resends the proposal as
/// each member answers, and opening again would reset its countdown.
struct ProposalShown {};
struct ProposalEnded { bool succeeded = false; };
struct RoleCheckStarted {};
struct RoleCheckFailed {};
struct RoleCheckUpdated {};
/// The role-check popup shows as the check begins and hides as it ends.
struct RoleCheckPopup { bool show = false; };
/// An update worth a line, said only when the state actually moved: the
/// server restates the queue every few seconds.
struct QueueNews { uint8_t updateType = 0; };
/// An update type this client has no name for; the state is left as it was.
struct UnknownQueueUpdate { uint8_t updateType = 0; bool extraInfo = false; bool queued = false; };
struct CompletionRewarded {};
struct BootUpdated {};
struct BootEnded { bool passed = false; };
/// The roles offered changed (LFG_ROLE_UPDATE): three frames draw the same
/// checkboxes and each ticks them from GetLFGRoles.
struct RolesChanged {};

using LfgChange = std::variant<QueueUpdated, JoinedQueue, JoinFailed, LockInfoReceived, QueueStatusUpdated,
                               ProposalOffered, ProposalFailed, ProposalSucceeded, ProposalUpdated,
                               ProposalShown, ProposalEnded, RoleCheckStarted, RoleCheckFailed,
                               RoleCheckUpdated, RoleCheckPopup, QueueNews, UnknownQueueUpdate,
                               CompletionRewarded, BootUpdated, BootEnded, RolesChanged>;

// The server's notices, which change nothing the finder holds.
struct TeleportDenied { TeleportRefusal reason = TeleportRefusal::Other; };
struct FinderDisabled {};
struct ContinueOffered { uint32_t dungeonId = 0; uint32_t typeId = 0; };
/// A group member picked roles for the check (LFG_ROLE_CHECK_ROLE_CHOSEN).
struct RoleChosen { std::string playerName; bool ready = false; uint32_t roles = 0; };

} // namespace wowee::lfg
