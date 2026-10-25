#include "group/domain/group.hpp"

namespace wowee::group {

Member* Group::findMember(uint64_t guid) {
    for (auto& m : roster_.members)
        if (m.guid == guid) return &m;
    return nullptr;
}

const Member* Group::member(uint64_t guid) const {
    for (const auto& m : roster_.members)
        if (m.guid == guid) return &m;
    return nullptr;
}

std::vector<GroupChange> Group::rosterReceived(Roster roster) {
    const uint8_t previousLoot = roster_.lootMethod;
    const bool wasInGroup = isInGroup();
    roster_ = std::move(roster);
    // The roster carries online state in isOnline; member stats carry it in
    // the onlineStatus bitmask. Seed the bitmask so both agree until the first
    // stats arrive - otherwise every member reads as offline.
    for (auto& m : roster_.members)
        if (m.isOnline) m.onlineStatus |= 0x0001;

    std::vector<GroupChange> changes;
    const bool nowInGroup = isInGroup();
    if (!nowInGroup && wasInGroup) {
        changes.emplace_back(NoLongerInGroup{});
    } else if (nowInGroup && !wasInGroup) {
        JoinedGroup joined;
        for (const auto& m : roster_.members) joined.members.push_back(m.name.empty() ? "?" : m.name);
        changes.emplace_back(std::move(joined));
    }
    if (wasInGroup && nowInGroup && roster_.lootMethod != previousLoot)
        changes.emplace_back(LootMethodChanged{roster_.lootMethod});
    changes.emplace_back(RosterReplaced{isRaid()});
    return changes;
}

std::vector<GroupChange> Group::disbanded() {
    roster_.members.clear();
    roster_.memberCount = 0;
    roster_.leaderGuid = 0;
    return {GroupDisbanded{}};
}

std::vector<GroupChange> Group::removed() {
    roster_ = Roster{};
    return {RemovedFromGroup{}};
}

std::vector<GroupChange> Group::left() {
    roster_ = Roster{};
    return {LeftGroup{}};
}

std::vector<GroupChange> Group::inviteReceived(const std::string& inviter, bool canAccept) {
    // A server sends the invite with canAccept clear to say the player was
    // invited and could not be offered it, being in a group already. Raising
    // the offer would leave an invite pending that the server never honours.
    if (!canAccept) return {InviteBlockedAlreadyGrouped{inviter}};
    invitePending_ = true;
    inviter_ = inviter;
    return {InviteReceived{inviter}};
}

void Group::inviteAnswered() {
    invitePending_ = false;
}

std::vector<GroupChange> Group::memberStatsReceived(const MemberStats& s, uint64_t playerGuid) {
    Member* m = findMember(s.guid);
    if (!m) return {};

    // isOnline is what the interface bindings read and onlineStatus what the
    // client's own panels read; both describe one fact, so they move together.
    const bool wasOnline = (m->onlineStatus & 0x0001) != 0;
    bool onlineChanged = false;
    if (s.present & StatsField::Status) {
        m->onlineStatus = s.onlineStatus;
        const bool nowOnline = (m->onlineStatus & 0x0001) != 0;
        m->isOnline = nowOnline ? 1 : 0;
        onlineChanged = nowOnline != wasOnline;
    }
    if (s.present & StatsField::CurHealth) m->curHealth = s.curHealth;
    if (s.present & StatsField::MaxHealth) m->maxHealth = s.maxHealth;
    if (s.present & StatsField::PowerType) m->powerType = s.powerType;
    if (s.present & StatsField::CurPower)  m->curPower = s.curPower;
    if (s.present & StatsField::MaxPower)  m->maxPower = s.maxPower;
    if (s.present & StatsField::Level)     m->level = s.level;
    if (s.present & StatsField::Zone)      m->zoneId = s.zoneId;
    if (s.present & StatsField::Position) {
        m->posX = s.posX;
        m->posY = s.posY;
    }
    m->hasPartyStats = true;
    return {MemberStatsChanged{s.guid, unitToken(s.guid, playerGuid), s.updateFlags, onlineChanged,
                               m->isOnline != 0}};
}

std::vector<GroupChange> Group::leaderNamed(const std::string& name) {
    for (const auto& m : roster_.members) {
        if (m.name == name) { roster_.leaderGuid = m.guid; break; }
    }
    return {LeaderChanged{name}};
}

std::vector<GroupChange> Group::reshaped(uint8_t groupType, uint8_t playerFlags,
                                         uint64_t leaderGuid, uint64_t playerGuid) {
    roster_.groupType = groupType;
    roster_.leaderGuid = leaderGuid;
    if (Member* me = findMember(playerGuid)) me->flags = playerFlags;
    return {GroupReshaped{}};
}

void Group::resetReadyCheck() {
    readyCount_ = 0;
    notReadyCount_ = 0;
    readyCheckResults_.clear();
}

std::vector<GroupChange> Group::readyCheckStarted(const std::string& initiator) {
    resetReadyCheck();
    readyCheckActive_ = true;
    readyCheckInitiator_ = initiator;
    if (readyCheckInitiator_.empty() && roster_.leaderGuid != 0) {
        if (const Member* leader = member(roster_.leaderGuid)) readyCheckInitiator_ = leader->name;
    }
    return {ReadyCheckStarted{readyCheckInitiator_}};
}

std::vector<GroupChange> Group::readyCheckAnswered(uint64_t guid, const std::string& name, bool ready) {
    if (ready) ++readyCount_; else ++notReadyCount_;
    if (!name.empty()) {
        bool found = false;
        for (auto& r : readyCheckResults_) {
            if (r.name == name) { r.ready = ready; found = true; break; }
        }
        if (!found) readyCheckResults_.push_back({name, ready});
    }
    return {ReadyCheckAnswered{guid, name, ready}};
}

std::vector<GroupChange> Group::readyCheckFinished() {
    ReadyCheckFinished finished{readyCount_, notReadyCount_};
    readyCheckActive_ = false;
    resetReadyCheck();
    return {finished};
}

std::vector<GroupChange> Group::marksReceived(bool fullList,
                                              const std::vector<std::pair<uint8_t, uint64_t>>& marks) {
    if (fullList) marks_.fill(0);
    for (const auto& [icon, guid] : marks)
        if (icon < kRaidMarkCount) marks_[icon] = guid;
    return {RaidMarksChanged{}};
}

std::vector<GroupChange> Group::markLocally(uint64_t guid, uint8_t icon) {
    if (icon != kNoMark && icon >= kRaidMarkCount) return {};
    for (auto& holder : marks_)
        if (holder == guid) holder = 0;
    if (icon != kNoMark) marks_[icon] = guid;
    return {RaidMarksChanged{}};
}

uint8_t Group::markOf(uint64_t guid) const {
    if (guid == 0) return kNoMark;
    for (uint32_t i = 0; i < kRaidMarkCount; ++i)
        if (marks_[i] == guid) return static_cast<uint8_t>(i);
    return kNoMark;
}

uint64_t Group::assigned(Assignment assignment) const {
    const uint8_t bit = assignment == Assignment::MainTank ? MemberFlag::MainTank : MemberFlag::MainAssist;
    for (const auto& m : roster_.members)
        if (m.flags & bit) return m.guid;
    return 0;
}

std::string Group::unitToken(uint64_t guid, uint64_t playerGuid) const {
    if (isRaid()) {
        for (size_t i = 0; i < roster_.members.size(); ++i)
            if (roster_.members[i].guid == guid) return "raid" + std::to_string(i + 1);
        return {};
    }
    int found = 0;
    for (const auto& m : roster_.members) {
        if (m.guid == playerGuid) continue;
        ++found;
        if (m.guid == guid) return "party" + std::to_string(found);
    }
    return {};
}

} // namespace wowee::group
