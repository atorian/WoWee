#include "game/group_defines.hpp"
#include "core/local_time.hpp"
#include "game/item_text.hpp"
#include "game/social_handler.hpp"
#include "game/contacts_gateway.hpp"
#include "game/who_gateway.hpp"
#include "game/duel_gateway.hpp"
#include "game/petition_gateway.hpp"
#include "game/arena_gateway.hpp"
#include "game/battleground_gateway.hpp"
#include "game/lfg_gateway.hpp"
#include "game/calendar_gateway.hpp"
#include "game/group_gateway.hpp"
#include "group/services/group.hpp"
#include "game/calendar_data.hpp"
#include "calendar/domain/records.hpp"
#include "calendar/services/calendar.hpp"
#include "lfg/domain/records.hpp"
#include "lfg/services/lfg.hpp"
#include "battleground/services/battlegrounds.hpp"
#include "arena/services/arena.hpp"
#include "petition/services/petitions.hpp"
#include "duel/services/duels.hpp"
#include "who/domain/who_list.hpp"
#include "who/services/who.hpp"
#include "contacts/services/contacts.hpp"
#include "game/game_events.hpp"
#include "game/character.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/packet_sender.hpp"
#include "game/ui_errors.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include "game/packet_parsers.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "network/world_connection.hpp"
#include "core/logger.hpp"
#include "core/cvar_store.hpp"
#include "core/event_bus.hpp"
#include "game/game_events.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace wowee {
namespace game {

namespace {

std::filesystem::path guildNameCachePath() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) {
        return std::filesystem::path(appData) / "wowee" / "guild_names.tsv";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".wowee" / "guild_names.tsv";
    }
#endif
    return std::filesystem::path("guild_names.tsv");
}

} // namespace




static bool parseInspectEquipmentPayload(network::Packet& packet,
                                         uint64_t& outGuid,
                                         std::array<uint32_t, 19>& outItems,
                                         std::array<uint16_t, 19>& outEnchants) {
    const size_t start = packet.getReadPos();
    constexpr size_t kGearBytes = 19 * sizeof(uint32_t);
    constexpr uint32_t kEquipmentSlotMask = (1u << 19) - 1u;

    auto reset = [&]() {
        packet.setReadPos(start);
        outGuid = 0;
        outItems.fill(0);
        outEnchants.fill(0);
    };

    auto countSlots = [](uint32_t mask) {
        int count = 0;
        for (int slot = 0; slot < 19; ++slot) {
            if (mask & (1u << slot)) ++count;
        }
        return count;
    };

    auto parseMaskedItems = [&](const char* guidEncoding) -> bool {
        if (!packet.hasRemaining(sizeof(uint32_t))) return false;
        const uint32_t slotMask = packet.readUInt32();
        if ((slotMask & ~kEquipmentSlotMask) != 0) return false;

        const int slotCount = countSlots(slotMask);
        if (slotCount <= 0) return false;

        const size_t remaining = packet.getRemainingSize();
        const size_t candidateRecordSizes[] = {24, 20, 16, 12, 8, 4};
        size_t recordSize = 0;
        for (size_t candidate : candidateRecordSizes) {
            if (remaining == static_cast<size_t>(slotCount) * candidate) {
                recordSize = candidate;
                break;
            }
        }
        if (recordSize == 0) {
            // Some cores append a tiny trailer. Prefer the largest plausible
            // record that fits cleanly, but do not consume packets that are
            // clearly not the masked inspect format.
            for (size_t candidate : candidateRecordSizes) {
                const size_t needed = static_cast<size_t>(slotCount) * candidate;
                if (remaining >= needed && remaining - needed <= 8) {
                    recordSize = candidate;
                    break;
                }
            }
        }
        if (recordSize < sizeof(uint32_t)) return false;

        int nonZero = 0;
        for (int slot = 0; slot < 19; ++slot) {
            if ((slotMask & (1u << slot)) == 0) continue;
            if (!packet.hasRemaining(recordSize)) return false;

            const size_t recordStart = packet.getReadPos();
            const uint32_t itemEntry = packet.readUInt32();
            uint16_t enchantId = 0;
            if (recordSize >= sizeof(uint32_t) + sizeof(uint16_t) &&
                packet.hasRemaining(sizeof(uint16_t))) {
                enchantId = packet.readUInt16();
            }
            packet.setReadPos(recordStart + recordSize);

            outItems[slot] = itemEntry;
            outEnchants[slot] = enchantId;
            if (itemEntry != 0) ++nonZero;
        }

        if (nonZero == 0) return false;
        LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE masked gear: guidEncoding=", guidEncoding,
                 " mask=0x", std::hex, slotMask, std::dec,
                 " slots=", slotCount, " recordSize=", recordSize,
                 " nonZero=", nonZero);
        return true;
    };

    auto readItems = [&]() -> bool {
        for (uint32_t& item : outItems) {
            if (!packet.hasRemaining(sizeof(uint32_t))) return false;
            item = packet.readUInt32();
        }
        return true;
    };

    reset();
    if (packet.hasRemaining(sizeof(uint64_t))) {
        outGuid = packet.readUInt64();
        if (outGuid != 0 && parseMaskedItems("uint64")) {
            return true;
        }
    }

    reset();
    if (packet.hasFullPackedGuid()) {
        outGuid = packet.readPackedGuid();
        if (outGuid != 0 && parseMaskedItems("packed")) {
            return true;
        }
    }

    reset();
    if (packet.getRemainingSize() >= sizeof(uint64_t) + kGearBytes) {
        outGuid = packet.readUInt64();
        if (outGuid != 0 && readItems()) {
            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE flat gear: guidEncoding=uint64 slots=19");
            return true;
        }
    }

    reset();
    if (packet.hasFullPackedGuid()) {
        outGuid = packet.readPackedGuid();
        if (outGuid != 0 && packet.getRemainingSize() >= kGearBytes && readItems()) {
            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE flat gear: guidEncoding=packed slots=19");
            return true;
        }
    }

    reset();
    return false;
}

static bool isTbcInspectTalentBitSet(const std::vector<uint8_t>& bitfield, uint32_t bitIndex) {
    const uint32_t slot = bitIndex / 7u;
    const uint32_t offset = bitIndex % 7u;
    return slot < bitfield.size() && (bitfield[slot] & (1u << offset)) != 0;
}

static bool decodeTbcInspectTalentBitfield(SocialQueries& queries,
                                           uint8_t classId,
                                           const std::vector<uint8_t>& bitfield,
                                           std::array<uint32_t, 3>& outTreePoints,
                                           uint32_t& outSpentTalents) {
    outTreePoints.fill(0);
    outSpentTalents = 0;
    if (classId == 0 || bitfield.empty()) return false;

    const uint32_t classMask = 1u << (classId - 1u);
    std::vector<const TalentTabEntry*> classTabs;
    for (const auto& [tabId, tab] : queries.talentTabs()) {
        if (tab.classMask & classMask) {
            classTabs.push_back(&tab);
        }
    }
    std::sort(classTabs.begin(), classTabs.end(),
              [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    if (classTabs.empty()) return false;

    uint32_t tabBitStart = 0;
    for (size_t tabIndex = 0; tabIndex < classTabs.size() && tabIndex < outTreePoints.size(); ++tabIndex) {
        std::vector<const TalentEntry*> talents;
        for (const auto& [talentId, talent] : queries.talents()) {
            if (talent.tabId == classTabs[tabIndex]->tabId && talent.maxRank > 0) {
                talents.push_back(&talent);
            }
        }
        std::sort(talents.begin(), talents.end(), [](const auto* a, const auto* b) {
            if (a->row != b->row) return a->row < b->row;
            if (a->column != b->column) return a->column < b->column;
            return a->talentId < b->talentId;
        });

        uint32_t tabBits = 0;
        for (const auto* talent : talents) {
            uint8_t rank = 0;
            for (uint8_t r = 1; r <= talent->maxRank; ++r) {
                if (isTbcInspectTalentBitSet(bitfield, tabBitStart + tabBits + (r - 1u))) {
                    rank = r;
                }
            }
            outTreePoints[tabIndex] += rank;
            outSpentTalents += rank;
            tabBits += talent->maxRank;
        }
        tabBitStart += tabBits;
    }

    return true;
}

static bool parseTbcInspectTalentPayload(network::Packet& packet,
                                         SocialQueries& queries,
                                         uint8_t classId,
                                         uint32_t& outUnspentTalents,
                                         uint32_t& outSpentTalents,
                                         std::array<uint32_t, 3>& outTreePoints,
                                         bool& outHasTreePoints) {
    const size_t start = packet.getReadPos();
    outUnspentTalents = 0;
    outSpentTalents = 0;
    outTreePoints.fill(0);
    outHasTreePoints = false;

    const size_t payloadSize = packet.getRemainingSize();
    if (payloadSize < sizeof(uint32_t)) {
        packet.setReadPos(start);
        return false;
    }

    auto parseTalentRecords = [](network::Packet& p, size_t count) {
        uint32_t spent = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t talentId = p.readUInt32();
            const uint8_t rank = p.readUInt8();
            if (talentId == 0) continue;
            spent += static_cast<uint32_t>(rank) + 1u;
        }
        return spent;
    };

    const uint32_t firstValue = packet.readUInt32();

    // TBC/CMaNGOS sends SMSG_INSPECT_TALENT as:
    //   uint32 byteCount, byte[byteCount] compact talent bitfield.
    // Older attempts treated byteCount (0x3d) as talent points; keep the
    // fallback list-style parser below for other 2.x cores, but prefer this
    // server-authored bitfield when the size lines up exactly.
    if (firstValue > 0 && firstValue <= 256 && packet.getRemainingSize() == firstValue) {
        std::vector<uint8_t> bitfield;
        bitfield.reserve(firstValue);
        for (uint32_t i = 0; i < firstValue; ++i) {
            bitfield.push_back(packet.readUInt8());
        }

        outHasTreePoints = decodeTbcInspectTalentBitfield(queries, classId, bitfield, outTreePoints, outSpentTalents);
        if (!outHasTreePoints) {
            outSpentTalents = 0;
        }
        LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed cmangos bitfield bytes=", firstValue,
                 " spent=", outSpentTalents,
                 " trees=", outTreePoints[0], "/", outTreePoints[1], "/", outTreePoints[2],
                 " class=", static_cast<int>(classId),
                 " decoded=", (outHasTreePoints ? "yes" : "no"),
                 " trailingBytes=", packet.getRemainingSize());
        return true;
    }

    // The regular talents-info packet uses uint32 unspent + uint8 count.
    // Accept that shape too so different 2.x cores still render something useful.
    if (packet.hasRemaining(1)) {
        const uint8_t nextByte = packet.readUInt8();
        const size_t afterHeaderBytes = packet.getRemainingSize();

        if (afterHeaderBytes > 0 && (afterHeaderBytes % 5u) == 0u) {
            const size_t recordCount = afterHeaderBytes / 5u;
            const uint32_t rankSpent = parseTalentRecords(packet, recordCount);
            outUnspentTalents = 0;
            outSpentTalents = firstValue != 0 ? firstValue : rankSpent;
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed cmangos talents records=", recordCount,
                     " spent=", outSpentTalents, " rankSpent=", rankSpent,
                     " unk=", static_cast<int>(nextByte),
                     " trailingBytes=", packet.getRemainingSize());
            return true;
        }

        const uint8_t talentCount = nextByte;
        if (packet.hasRemaining(static_cast<size_t>(talentCount) * 5u)) {
            const uint32_t spentTalents = parseTalentRecords(packet, talentCount);
            outUnspentTalents = firstValue;
            outSpentTalents = spentTalents;
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed talents count=", static_cast<int>(talentCount),
                     " spent=", spentTalents, " unspent=", firstValue,
                     " trailingBytes=", packet.getRemainingSize());
            return true;
        }
    }

    if ((payloadSize % 5u) == 0u) {
        packet.setReadPos(start);
        const size_t recordCount = payloadSize / 5u;
        outSpentTalents = parseTalentRecords(packet, recordCount);
        LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed bare talents records=", recordCount,
                 " spent=", outSpentTalents,
                 " trailingBytes=", packet.getRemainingSize());
        return true;
    }

    packet.setReadPos(start);
    return false;
}

static const std::string kEmptyString;

// The contact lists the protocol feeds. A client built without them drops
// the contact packets, and says so once.
static who::IWhoFeed* whoFeed(SocialQueries& queries) {
    auto* feed = queries.whoFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("/who is not wired into this client; SMSG_WHO answers are dropped");
    return feed;
}

static duel::IDuelsFeed* duelFeed(SocialQueries& queries) {
    auto* feed = queries.duelFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("Duels are not wired into this client; duel packets are dropped and no challenge is shown");
    return feed;
}

static petition::IPetitionsFeed* petitionFeed(SocialQueries& queries) {
    auto* feed = queries.petitionFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("Charters are not wired into this client; petition packets are dropped and no registrar opens");
    return feed;
}

static arena::IArenaTeamsFeed* arenaFeed(SocialQueries& queries) {
    auto* feed = queries.arenaFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("Arena teams are not wired into this client; arena team packets are dropped");
    return feed;
}

static battleground::IBattlegroundsFeed* battlegroundFeed(SocialQueries& queries) {
    auto* feed = queries.battlegroundFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("Battlegrounds are not wired into this client; battleground packets are dropped and no queue is shown");
    return feed;
}

static lfg::ILfgFeed* lfgFeed(SocialQueries& queries) {
    auto* feed = queries.lfgFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("The dungeon finder is not wired into this client; its packets are dropped and no queue is shown");
    return feed;
}

static calendar::ICalendarFeed* calendarFeed(SocialQueries& queries) {
    auto* feed = queries.calendarFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("The calendar is not wired into this client; calendar packets are dropped and it stays empty");
    return feed;
}

static group::IGroupServerFeed* groupFeed(SocialQueries& queries) {
    auto* feed = queries.groupFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("The group is not wired into this client; party and raid packets are dropped");
    return feed;
}

static contacts::IContactsServerFeed* contactsFeed(SocialQueries& queries) {
    auto* feed = queries.contactsFeed();
    static bool said = false;
    if (!feed && !std::exchange(said, true))
        LOG_WARNING("Contact lists are not wired into this client; friend, ignore and mute packets are dropped");
    return feed;
}

SocialHandler::SocialHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                             ChatLog& chat, SocialQueries& queries, const core::CVarStore& cvars)
    : sender_(sender), frameXml_(frameXml), events_(events), chat_(chat), queries_(queries),
      cvars_(cvars) {
    loadGuildNameCache();
    // A friend or an ignored player whose name was not known when the list
    // came is filed under it once it is.
    subscriptions_.push_back(events_.subscribe<events::PlayerNameKnown>(
        [this](const events::PlayerNameKnown& e) {
            if (auto* feed = contactsFeed(queries_)) feed->onPlayerNameResolved(e.guid, e.name);
        }));
    // What the player sees of the contact lists: chat lines and the
    // friends panel's events.
    for (auto& subscription : subscribeContactNotices(events_, chat_, frameXml_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribeWhoNotices(events_, chat_, frameXml_, queries_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribeDuelNotices(events_, chat_, frameXml_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribePetitionNotices(events_, chat_, frameXml_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribeArenaNotices(events_, chat_, frameXml_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribeBattlegroundNotices(events_, chat_, frameXml_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribeLfgNotices(events_, chat_, frameXml_, queries_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribeCalendarNotices(events_, chat_, frameXml_, queries_))
        subscriptions_.push_back(std::move(subscription));
    for (auto& subscription : subscribeGroupNotices(events_, chat_, frameXml_))
        subscriptions_.push_back(std::move(subscription));
    // The character's social state is the server's to say at every login:
    // SMSG_GROUP_LIST comes with it for a character in a party, the guild
    // and the queues with theirs. Kept, the last session's party stood for
    // the next character (and for this one back from a lost connection after
    // the group had gone on without it), its guild made a guildless one look
    // guilded, and a duel or an invite put to it waited for the next one.
    subscriptions_.push_back(events.subscribe<events::CharacterLoggingIn>(
        [this](const events::CharacterLoggingIn&) {
            static_cast<SocialCharacterState&>(*this) = SocialCharacterState{};
            if (auto* feed = contactsFeed(queries_)) feed->beginCharacter();
            if (auto* feed = whoFeed(queries_)) feed->beginCharacter();
            if (auto* feed = duelFeed(queries_)) feed->beginCharacter();
            if (auto* feed = petitionFeed(queries_)) feed->beginCharacter();
            if (auto* feed = arenaFeed(queries_)) feed->beginCharacter();
            if (auto* feed = battlegroundFeed(queries_)) feed->beginCharacter();
            if (auto* feed = lfgFeed(queries_)) feed->beginCharacter();
            if (auto* feed = calendarFeed(queries_)) feed->beginCharacter();
            if (auto* feed = groupFeed(queries_)) feed->beginCharacter();
        }));
}

void SocialHandler::loadGuildNameCache() {
    std::ifstream in(guildNameCachePath());
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.find('\t');
        if (separator == std::string::npos) continue;

        try {
            const uint32_t guildId = static_cast<uint32_t>(std::stoul(line.substr(0, separator)));
            std::string guildName = line.substr(separator + 1);
            if (guildId != 0 && !guildName.empty()) {
                guildNameCache_[guildId] = std::move(guildName);
            }
        } catch (...) {
            // Ignore stale or hand-edited cache lines.
        }
    }
}

void SocialHandler::saveGuildNameCache() const {
    const std::filesystem::path path = guildNameCachePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save guild name cache to ", path.string());
        return;
    }

    for (const auto& [guildId, guildName] : guildNameCache_) {
        if (guildId != 0 && !guildName.empty()) {
            out << guildId << '\t' << guildName << '\n';
        }
    }
}

void SocialHandler::rememberGuildName(uint32_t guildId, const std::string& guildName) {
    if (guildId == 0 || guildName.empty()) return;

    auto it = guildNameCache_.find(guildId);
    if (it != guildNameCache_.end() && it->second == guildName) return;

    guildNameCache_[guildId] = guildName;
    saveGuildNameCache();
}

// ============================================================
// registerOpcodes
// ============================================================

void SocialHandler::raiseUiError(const std::string& message) {
    game::raiseUiError(events_, frameXml_, message);
}

void SocialHandler::ageMinimapPings(float deltaTime) {
    for (auto& p : minimapPings_) p.age += deltaTime;
    minimapPings_.erase(std::remove_if(minimapPings_.begin(), minimapPings_.end(),
                                       [](const MinimapPing& p) { return p.isExpired(); }),
                        minimapPings_.end());
}

void SocialHandler::registerOpcodes(DispatchTable& table) {
    registerBattlefieldOpcodes(table);

    // ---- Player info queries / social ----
    table[Opcode::SMSG_QUERY_TIME_RESPONSE] = [this](network::Packet& packet) {
        if (sender_.characterInWorld()) handleQueryTimeResponse(packet);
    };
    table[Opcode::SMSG_PLAYED_TIME] = [this](network::Packet& packet) {
        if (sender_.characterInWorld()) handlePlayedTime(packet);
    };
    table[Opcode::SMSG_WHO] = [this](network::Packet& packet) {
        if (!sender_.characterInWorld()) return;
        // /who belongs to the who context; this only decodes.
        const bool hasGender = isActiveExpansion("wotlk");
        const uint32_t found = packet.readUInt32();
        const uint32_t onlineCount = packet.readUInt32();
        std::vector<who::WhoEntry> entries;
        for (uint32_t i = 0; i < found; ++i) {
            if (packet.getReadPos() >= packet.getSize()) break;
            who::WhoEntry entry;
            entry.name = packet.readString();
            entry.guildName = packet.readString();
            if (!packet.hasRemaining(12)) break;
            entry.level   = packet.readUInt32();
            entry.classId = packet.readUInt32();
            entry.raceId  = packet.readUInt32();
            if (hasGender && packet.hasRemaining(1)) packet.readUInt8();
            if (packet.hasRemaining(4)) entry.zoneId = packet.readUInt32();
            entries.push_back(std::move(entry));
        }
        if (auto* feed = whoFeed(queries_)) feed->onAnswer(found, onlineCount, std::move(entries));
    };
    table[Opcode::SMSG_WHOIS] = [this](network::Packet& packet) {
        if (packet.getReadPos() < packet.getSize()) {
            std::string whoisText = packet.readString();
            if (!whoisText.empty()) {
                std::string line;
                for (char c : whoisText) {
                    if (c == '\n') { if (!line.empty()) chat_.addSystemChatMessage("[Whois] " + line); line.clear(); }
                    else line += c;
                }
                if (!line.empty()) chat_.addSystemChatMessage("[Whois] " + line);
                LOG_INFO("SMSG_WHOIS: ", whoisText);
            }
        }
    };
    // The contact lists belong to the contacts context; these only decode.
    table[Opcode::SMSG_FRIEND_STATUS] = [this](network::Packet& packet) {
        if (!sender_.characterInWorld()) return;
        FriendStatusData data;
        if (!FriendStatusParser::parse(packet, data)) return;
        if (auto* feed = contactsFeed(queries_)) feed->onFriendStatus(toFriendStatusUpdate(data));
    };
    table[Opcode::SMSG_CONTACT_LIST] = [this](network::Packet& packet) {
        uint32_t mask = 0;
        auto records = readContactList(packet, mask);
        if (auto* feed = contactsFeed(queries_); feed && records) feed->onContactList(std::move(*records), mask);
    };
    table[Opcode::SMSG_FRIEND_LIST] = [this](network::Packet& packet) {
        auto records = readFriendList(packet);
        if (auto* feed = contactsFeed(queries_); feed && records) feed->onFriendList(std::move(*records));
    };
    table[Opcode::SMSG_IGNORE_LIST] = [this](network::Packet& packet) {
        auto guids = readIgnoreList(packet);
        if (auto* feed = contactsFeed(queries_); feed && guids) feed->onIgnoreList(*guids);
    };
    table[Opcode::MSG_RANDOM_ROLL] = [this](network::Packet& packet) {
        if (sender_.characterInWorld()) handleRandomRoll(packet);
    };

    // ---- Logout ----
    table[Opcode::SMSG_LOGOUT_RESPONSE] = [this](network::Packet& packet) { handleLogoutResponse(packet); };
    table[Opcode::SMSG_LOGOUT_COMPLETE] = [this](network::Packet& packet) { handleLogoutComplete(packet); };

    // ---- Inspect ----
    table[Opcode::SMSG_INSPECT_TALENT] = [this](network::Packet& packet) { handleInspectResults(packet); };
    // The honour tab's own answer, on the same opcode it was asked with.
    // AzerothCore replies with the target's guid, the honour byte, the packed
    // kill counter and three totals - MiscHandler.cpp, HandleInspectHonorStats.
    table[Opcode::MSG_INSPECT_HONOR_STATS] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8 + 1 + 16)) return;
        const uint64_t guid = packet.readUInt64();
        // Only for whoever is being inspected. The window reads one result and
        // a stale answer for someone else would be attributed to them.
        if (inspectResult_.guid != 0 && guid != inspectResult_.guid) return;
        inspectResult_.honorRank = packet.readUInt8();
        // One field holds both days: PAIR32_LOPART is today and the high half
        // is yesterday, which is how the server rolls the counter over -
        // MAKE_PAIR32(0, kills_today) at midnight.
        const uint32_t kills = packet.readUInt32();
        inspectResult_.honorTodayKills     = kills & 0xFFFFu;
        inspectResult_.honorYesterdayKills = kills >> 16;
        inspectResult_.honorTodayContribution     = packet.readUInt32();
        inspectResult_.honorYesterdayContribution = packet.readUInt32();
        inspectResult_.honorLifetimeKills         = packet.readUInt32();
        inspectResult_.hasHonorData = true;
        if (frameXml_.frameXmlListening())
            frameXml_.fireFrameXmlEvent("INSPECT_HONOR_UPDATE", {});
    };
    table[Opcode::SMSG_INSPECT_RESULTS_UPDATE] = [this](network::Packet& packet) { handleInspectResults(packet); };

    // ---- Group ----
    table[Opcode::SMSG_GROUP_INVITE] = [this](network::Packet& packet) { handleGroupInvite(packet); };
    table[Opcode::SMSG_GROUP_DECLINE] = [this](network::Packet& packet) {
        GroupDeclineData data;
        if (!GroupDeclineResponseParser::parse(packet, data)) return;
        if (auto* feed = groupFeed(queries_)) feed->onInviteDeclined(data.playerName);
    };
    table[Opcode::SMSG_GROUP_LIST] = [this](network::Packet& packet) { handleGroupList(packet); };
    table[Opcode::SMSG_GROUP_DESTROYED] = [this](network::Packet& /*packet*/) {
        if (auto* feed = groupFeed(queries_)) feed->onDisbanded();
    };
    table[Opcode::SMSG_GROUP_CANCEL] = [this](network::Packet& /*packet*/) {
        if (auto* feed = groupFeed(queries_)) feed->onInviteCancelled();
    };
    table[Opcode::SMSG_GROUP_UNINVITE] = [this](network::Packet& /*packet*/) {
        if (auto* feed = groupFeed(queries_)) feed->onRemoved();
    };
    table[Opcode::SMSG_PARTY_COMMAND_RESULT] = [this](network::Packet& packet) {
        PartyCommandResultData data;
        if (!PartyCommandResultParser::parse(packet, data)) return;
        if (auto* feed = groupFeed(queries_)) feed->onCommandResult(data.result, data.name);
    };
    table[Opcode::SMSG_PARTY_MEMBER_STATS] = [this](network::Packet& packet) { handlePartyMemberStats(packet, false); };
    table[Opcode::SMSG_PARTY_MEMBER_STATS_FULL] = [this](network::Packet& packet) { handlePartyMemberStats(packet, true); };
    table[Opcode::SMSG_GROUP_SET_LEADER] = [this](network::Packet& packet) {
        if (!packet.hasData()) return;
        const std::string leaderName = packet.readString();
        if (auto* feed = groupFeed(queries_)) feed->onLeaderNamed(leaderName);
    };
    // The group's type, its leader and the player's own member flags, without
    // a new list.
    table[Opcode::SMSG_REAL_GROUP_UPDATE] = [this](network::Packet& packet) {
        auto rem = [&]() { return packet.getRemainingSize(); };
        if (rem() < 1) return;
        const uint8_t groupType = packet.readUInt8();
        if (rem() < 4) return;
        const uint32_t memberFlags = packet.readUInt32();
        if (rem() < 8) return;
        const uint64_t leaderGuid = packet.readUInt64();
        LOG_DEBUG("SMSG_REAL_GROUP_UPDATE groupType=", static_cast<int>(groupType),
                  " memberFlags=0x", std::hex, memberFlags, std::dec, " leaderGuid=", leaderGuid);
        if (auto* feed = groupFeed(queries_))
            feed->onReshaped(groupType, static_cast<uint8_t>(memberFlags & 0xFF), leaderGuid);
    };
    // The raid target markers. The full list is authoritative, so icons absent
    // from it are gone.
    table[Opcode::MSG_RAID_TARGET_UPDATE] = [this](network::Packet& packet) {
        RaidTargetUpdateData rtu;
        if (!RaidTargetUpdateParser::parse(packet, rtu)) return;
        LOG_DEBUG("MSG_RAID_TARGET_UPDATE: fullList=", rtu.fullList, " marks=", rtu.marks.size());
        if (auto* feed = groupFeed(queries_)) feed->onRaidMarks(rtu.fullList, rtu.marks);
    };

    // ---- Ready check ----
    table[Opcode::MSG_RAID_READY_CHECK] = [this](network::Packet& packet) {
        // The initiator by the unit in view; the group names its leader when
        // there is none.
        std::string initiator;
        if (packet.hasRemaining(8)) {
            const uint64_t initiatorGuid = packet.readUInt64();
            auto entity = queries_.entities().getEntity(initiatorGuid);
            if (auto* unit = dynamic_cast<Unit*>(entity.get())) initiator = unit->getName();
        }
        if (auto* feed = groupFeed(queries_)) feed->onReadyCheckStarted(initiator);
    };
    table[Opcode::MSG_RAID_READY_CHECK_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
        const uint64_t respGuid = packet.readUInt64();
        const bool isReady = packet.readUInt8() != 0;
        std::string name = queries_.playerName(respGuid);
        if (name.empty()) {
            // Only cast to Unit if the entity actually is one - a raw
            // static_pointer_cast on a GameObject would be undefined behavior.
            auto ent = queries_.entities().getEntity(respGuid);
            if (ent && (ent->getType() == ObjectType::UNIT || ent->getType() == ObjectType::PLAYER))
                name = std::static_pointer_cast<game::Unit>(ent)->getName();
        }
        if (auto* feed = groupFeed(queries_)) feed->onReadyCheckAnswer(respGuid, name, isReady);
    };
    table[Opcode::MSG_RAID_READY_CHECK_FINISHED] = [this](network::Packet& /*packet*/) {
        if (auto* feed = groupFeed(queries_)) feed->onReadyCheckFinished();
    };
    table[Opcode::SMSG_RAID_INSTANCE_INFO] = [this](network::Packet& packet) { handleRaidInstanceInfo(packet); };

    // ---- Duels ----
    table[Opcode::SMSG_DUEL_REQUESTED] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) { packet.skipAll(); return; }
        // The flag first, then the challenger. Read the other way round, the
        // challenger was the flag - a game object no name lookup could find, so
        // the challenge came from a hex number - and the flag, which the accept
        // and the refusal both have to name, was thrown away.
        const uint64_t arbiter = packet.readUInt64();
        const uint64_t challenger = packet.readUInt64();
        if (auto* feed = duelFeed(queries_)) feed->onRequested(arbiter, challenger);
    };
    table[Opcode::SMSG_DUEL_COMPLETE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        const bool started = packet.readUInt8() != 0;
        if (auto* feed = duelFeed(queries_)) feed->onComplete(started);
    };
    table[Opcode::SMSG_DUEL_WINNER] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(3)) return;
        const bool fled = packet.readUInt8() == 1;
        std::string winner = packet.readString();
        std::string loser = packet.readString();
        if (auto* feed = duelFeed(queries_)) feed->onWinner(fled, std::move(winner), std::move(loser));
    };
    table[Opcode::SMSG_DUEL_OUTOFBOUNDS] = [this](network::Packet& /*packet*/) {
        if (auto* feed = duelFeed(queries_)) feed->onOutOfBounds();
    };
    table[Opcode::SMSG_DUEL_INBOUNDS] = [this](network::Packet& /*packet*/) {
        if (auto* feed = duelFeed(queries_)) feed->onInBounds();
    };
    table[Opcode::SMSG_DUEL_COUNTDOWN] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const uint32_t ms = packet.readUInt32();
        if (auto* feed = duelFeed(queries_)) feed->onCountdown(ms);
    };
    table[Opcode::SMSG_PARTYKILLLOG] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) return;
        uint64_t killerGuid = packet.readUInt64();
        uint64_t victimGuid = packet.readUInt64();
        auto nameFor = [this](uint64_t g) -> std::string {
            if (std::string name = queries_.playerName(g); !name.empty()) return name;
            auto ent = queries_.entities().getEntity(g);
            if (ent && (ent->getType() == game::ObjectType::UNIT ||
                        ent->getType() == game::ObjectType::PLAYER))
                return std::static_pointer_cast<game::Unit>(ent)->getName();
            return {};
        };
        std::string killerName = nameFor(killerGuid);
        std::string victimName = nameFor(victimGuid);
        if (!killerName.empty() && !victimName.empty()) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s killed %s.", killerName.c_str(), victimName.c_str());
            chat_.addSystemChatMessage(buf);
        }
    };

    // ---- Guild ----
    table[Opcode::SMSG_GUILD_INFO] = [this](network::Packet& packet) { handleGuildInfo(packet); };
    table[Opcode::SMSG_GUILD_ROSTER] = [this](network::Packet& packet) { handleGuildRoster(packet); };
    table[Opcode::SMSG_GUILD_QUERY_RESPONSE] = [this](network::Packet& packet) { handleGuildQueryResponse(packet); };
    table[Opcode::SMSG_GUILD_EVENT] = [this](network::Packet& packet) { handleGuildEvent(packet); };
    table[Opcode::SMSG_GUILD_INVITE] = [this](network::Packet& packet) { handleGuildInvite(packet); };
    table[Opcode::SMSG_GUILD_COMMAND_RESULT] = [this](network::Packet& packet) { handleGuildCommandResult(packet); };
    table[Opcode::SMSG_PETITION_SHOWLIST] = [this](network::Packet& packet) { handlePetitionShowlist(packet); };
    table[Opcode::SMSG_TURN_IN_PETITION_RESULTS] = [this](network::Packet& packet) {
        uint32_t result = 0;
        if (!TurnInPetitionResultsParser::parse(packet, result)) return;
        auto* feed = petitionFeed(queries_);
        if (!feed) return;
        if (result == 0) feed->onTurnedIn();
        else feed->onTurnInRefused(turnInRefusal(result), result);
    };
    table[Opcode::SMSG_OFFER_PETITION_ERROR] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const uint32_t err = packet.readUInt32();
        if (auto* feed = petitionFeed(queries_)) feed->onOfferRefused(offerRefusal(err));
    };
    table[Opcode::MSG_GUILD_EVENT_LOG_QUERY] = [this](network::Packet& packet) { handleGuildEventLog(packet); };
    table[Opcode::MSG_GUILD_BANK_LOG_QUERY] = [this](network::Packet& packet) { handleGuildBankLog(packet); };
    table[Opcode::SMSG_PETITION_QUERY_RESPONSE] = [this](network::Packet& packet) { handlePetitionQueryResponse(packet); };
    table[Opcode::SMSG_PETITION_SHOW_SIGNATURES] = [this](network::Packet& packet) { handlePetitionShowSignatures(packet); };
    table[Opcode::SMSG_PETITION_SIGN_RESULTS] = [this](network::Packet& packet) { handlePetitionSignResults(packet); };

    // ---- Battlefield / BG ----
    table[Opcode::SMSG_BATTLEFIELD_STATUS] = [this](network::Packet& packet) {
        battleground::BattlefieldStatus status;
        if (!BattlefieldStatusPacket::parse(packet, status, isClassicLikeExpansion(), !isPreWotlk())) return;
        if (auto* feed = battlegroundFeed(queries_)) feed->onStatus(status);
    };
    table[Opcode::SMSG_BATTLEFIELD_LIST] = [this](network::Packet& packet) {
        auto info = readBattlefieldList(packet);
        if (!info) return;
        if (auto* feed = battlegroundFeed(queries_)) feed->onList(std::move(*info));
    };
    table[Opcode::SMSG_BATTLEFIELD_PORT_DENIED] = [this](network::Packet& /*packet*/) {
        if (auto* feed = battlegroundFeed(queries_)) feed->onPortDenied();
    };
    table[Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS] = [this](network::Packet& packet) {
        auto positions = readBattlegroundPositions(packet);
        if (auto* feed = battlegroundFeed(queries_)) feed->onPositions(std::move(positions));
    };
    table[Opcode::SMSG_REMOVED_FROM_PVP_QUEUE] = [this](network::Packet& /*packet*/) {
        if (auto* feed = battlegroundFeed(queries_)) feed->onRemovedFromQueue();
    };
    // The answer to a join, and most often a refusal. A positive value is the
    // battleground joined; the negative ones are the reasons a queue was
    // refused. It was read as "joined" whatever it said, so a Deserter or a
    // third queue was reported as success and nothing appeared on the minimap.
    table[Opcode::SMSG_GROUP_JOINED_BATTLEGROUND] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const int32_t result = static_cast<int32_t>(packet.readUInt32());
        auto* feed = battlegroundFeed(queries_);
        if (!feed) return;
        if (result == -1) { feed->onJoinAcknowledged(); return; }  // nothing to say
        if (result > 0) { feed->onGroupQueued(static_cast<uint32_t>(result)); return; }
        std::optional<uint64_t> member;
        if (result == -11 && packet.hasRemaining(8)) member = packet.readUInt64();
        feed->onJoinRefused(joinRefusal(result), result, member);
    };
    table[Opcode::SMSG_JOINED_BATTLEGROUND_QUEUE] = [this](network::Packet& /*packet*/) {
        if (auto* feed = battlegroundFeed(queries_)) feed->onJoinedQueue();
    };
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        const uint64_t guid = packet.readUInt64();
        if (auto* feed = battlegroundFeed(queries_)) feed->onPlayerEntered(guid);
    };
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        const uint64_t guid = packet.readUInt64();
        if (auto* feed = battlegroundFeed(queries_)) feed->onPlayerLeft(guid);
    };

    // ---- Calendar ----
    // One event in full, in answer to opening it.
    table[Opcode::SMSG_CALENDAR_SEND_EVENT] = [this](network::Packet& packet) {
        calendar::CalendarEventDetail parsed;
        if (!parseCalendarSendEvent(packet, parsed)) {
            LOG_WARNING("SMSG_CALENDAR_SEND_EVENT: could not be read whole; "
                        "keeping the previous event rather than a partial one");
            packet.skipAll();
            return;
        }
        LOG_INFO("SMSG_CALENDAR_SEND_EVENT: '", parsed.title, "' with ", parsed.invitees.size(), " invitee(s)");
        packet.skipAll();
        if (auto* feed = calendarFeed(queries_)) feed->onEvent(std::move(parsed));
    };
    // The whole calendar, in answer to a request for it.
    //
    // Six lists back to back with no length prefix on any of them, so this is
    // parsed in one place with a test rather than read inline - a row taken
    // one field wrong slides everything after it and the next count is read
    // out of the middle of a string, which fails silently and completely.
    // See parseCalendarSendCalendar.
    table[Opcode::SMSG_CALENDAR_SEND_CALENDAR] = [this](network::Packet& packet) {
        calendar::CalendarData parsed;
        if (!parseCalendarSendCalendar(packet, parsed)) {
            LOG_WARNING("SMSG_CALENDAR_SEND_CALENDAR: could not be read whole; "
                        "keeping the previous calendar rather than a partial one");
            packet.skipAll();
            return;
        }
        LOG_INFO("SMSG_CALENDAR_SEND_CALENDAR: ", parsed.events.size(), " event(s), ",
                 parsed.invites.size(), " invite(s), ", parsed.lockouts.size(), " raid lockout(s), ",
                 parsed.holidays.size(), " holiday(s)");
        packet.skipAll();
        if (auto* feed = calendarFeed(queries_)) feed->onCalendar(std::move(parsed));
    };
    // uint32 numPending - the number of unanswered calendar invites.
    table[Opcode::SMSG_CALENDAR_SEND_NUM_PENDING] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const uint32_t numPending = packet.readUInt32();
        LOG_DEBUG("SMSG_CALENDAR_SEND_NUM_PENDING: ", numPending, " pending invites");
        if (auto* feed = calendarFeed(queries_)) feed->onPendingCount(numPending);
    };
    // uint32 command + uint8 result + cstring info. Zero is success, which the
    // server answers with nothing else to say.
    table[Opcode::SMSG_CALENDAR_COMMAND_RESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
        /*uint32_t command =*/ packet.readUInt32();
        const uint8_t result = packet.readUInt8();
        packet.skipAll();
        if (result == 0) return;
        if (auto* feed = calendarFeed(queries_)) feed->onCommandRefused(calendarCommandRefusal(result));
    };
    table[Opcode::SMSG_CALENDAR_EVENT_INVITE_ALERT] = [this](network::Packet& packet) {
        // eventId(8) + title(cstring) + eventTime(4, packed) + flags(4) +
        // eventType(4) + dungeonId(4, signed) + inviteId(8) + status(1) +
        // rank(1) + creatorGuid(packed) + senderGuid(packed).
        //
        // Only the id and the title are read; the rest is skipped.
        if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
        /*uint64_t eventId =*/ packet.readUInt64();
        std::string title = packet.hasData() ? packet.readString() : "";
        packet.skipAll();
        LOG_INFO("SMSG_CALENDAR_EVENT_INVITE_ALERT: title='", title, "'");
        if (auto* feed = calendarFeed(queries_)) feed->onInviteAlert(std::move(title));
    };
    // The player's own answer to an invitation changed.
    // inviteId(8) + eventId(8) + eventType(1) + flags(4) + inviteTime(8) +
    // status(1) + rank(1) + isGuildEvent(1) + title(cstring)
    table[Opcode::SMSG_CALENDAR_EVENT_STATUS] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(31)) { packet.skipAll(); return; }
        /*uint64_t inviteId =*/ packet.readUInt64();
        /*uint64_t eventId  =*/ packet.readUInt64();
        /*uint8_t  evType   =*/ packet.readUInt8();
        /*uint32_t flags    =*/ packet.readUInt32();
        /*uint64_t invTime  =*/ packet.readUInt64();
        const uint8_t status = packet.readUInt8();
        /*uint8_t rank      =*/ packet.readUInt8();
        /*uint8_t isGuild   =*/ packet.readUInt8();
        std::string title = packet.hasData() ? packet.readString() : "";
        packet.skipAll();
        if (auto* feed = calendarFeed(queries_)) feed->onInviteStatus(status, std::move(title));
    };
    // uint64 inviteId + uint64 eventId + uint32 mapId + uint32 difficulty + uint64 resetTime
    table[Opcode::SMSG_CALENDAR_RAID_LOCKOUT_ADDED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(28)) {
            /*uint64_t inviteId =*/ packet.readUInt64();
            /*uint64_t eventId  =*/ packet.readUInt64();
            const uint32_t mapId = packet.readUInt32();
            const uint32_t difficulty = packet.readUInt32();
            /*uint64_t resetTime =*/ packet.readUInt64();
            LOG_DEBUG("SMSG_CALENDAR_RAID_LOCKOUT_ADDED: mapId=", mapId, " difficulty=", difficulty);
            if (auto* feed = calendarFeed(queries_)) feed->onLockoutAdded(mapId, difficulty);
        }
        packet.skipAll();
    };
    // uint64 inviteId + uint64 eventId + uint32 mapId + uint32 difficulty
    table[Opcode::SMSG_CALENDAR_RAID_LOCKOUT_REMOVED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(20)) {
            /*uint64_t inviteId =*/ packet.readUInt64();
            /*uint64_t eventId  =*/ packet.readUInt64();
            const uint32_t mapId = packet.readUInt32();
            const uint32_t difficulty = packet.readUInt32();
            LOG_DEBUG("SMSG_CALENDAR_RAID_LOCKOUT_REMOVED: mapId=", mapId, " difficulty=", difficulty);
            if (auto* feed = calendarFeed(queries_)) feed->onLockoutRemoved(mapId, difficulty);
        }
        packet.skipAll();
    };

    // ---- Instance ----
    for (auto op : { Opcode::SMSG_INSTANCE_DIFFICULTY, Opcode::MSG_SET_DUNGEON_DIFFICULTY }) {
        table[op] = [this](network::Packet& packet) { handleInstanceDifficulty(packet); };
    }

    // ---- Guild / RAF / PvP AFK (moved from GameHandler) ----
    table[Opcode::SMSG_GUILD_DECLINE] = [this](network::Packet& packet) {
        if (packet.hasData()) {
            std::string name = packet.readString();
            chat_.addSystemChatMessage(name + " declined your guild invitation.");
        }
    };
    table[Opcode::SMSG_REFER_A_FRIEND_EXPIRED] = [this](network::Packet& packet) {
        chat_.addSystemChatMessage("Your Recruit-A-Friend link has expired.");
        packet.skipAll();
    };
    table[Opcode::SMSG_REFER_A_FRIEND_FAILURE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t reason = packet.readUInt32();
            static const char* kRafErrors[] = {
                "Not eligible",            // 0
                "Target not eligible",     // 1
                "Too many referrals",      // 2
                "Wrong faction",           // 3
                "Not a recruit",           // 4
                "Recruit requirements not met", // 5
                "Level above requirement", // 6
                "Friend needs account upgrade", // 7
            };
            const char* msg = (reason < 8) ? kRafErrors[reason]
                                           : "Recruit-A-Friend failed.";
            chat_.addSystemChatMessage(std::string("Recruit-A-Friend: ") + msg);
        }
        packet.skipAll();
    };
    table[Opcode::SMSG_REPORT_PVP_AFK_RESULT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            const uint8_t result = packet.readUInt8();
            if (auto* feed = battlegroundFeed(queries_)) feed->onAfkReportResult(result);
        }
        packet.skipAll();
    };
    table[Opcode::SMSG_INSTANCE_SAVE_CREATED] = [this](network::Packet& /*packet*/) {
        chat_.addSystemChatMessage("You are now saved to this instance.");
    };
    table[Opcode::SMSG_RAID_INSTANCE_MESSAGE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) return;
        uint32_t msgType = packet.readUInt32();
        uint32_t mapId   = packet.readUInt32();
        packet.readUInt32(); // diff
        std::string mapLabel = queries_.mapName(mapId);
        if (mapLabel.empty()) mapLabel = "instance #" + std::to_string(mapId);
        if (msgType == 1 && packet.hasRemaining(4)) {
            uint32_t timeLeft = packet.readUInt32();
            chat_.addSystemChatMessage(mapLabel + " will reset in " + std::to_string(timeLeft / 60) + " minute(s).");
        } else if (msgType == 2) {
            chat_.addSystemChatMessage("You have been saved to " + mapLabel + ".");
        } else if (msgType == 3) {
            chat_.addSystemChatMessage("Welcome to " + mapLabel + ".");
        }
    };
    table[Opcode::SMSG_INSTANCE_RESET] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t mapId = packet.readUInt32();
        auto it = std::remove_if(instanceLockouts_.begin(), instanceLockouts_.end(),
            [mapId](const InstanceLockout& lo){ return lo.mapId == mapId; });
        instanceLockouts_.erase(it, instanceLockouts_.end());
        std::string mapLabel = queries_.mapName(mapId);
        if (mapLabel.empty()) mapLabel = "instance #" + std::to_string(mapId);
        chat_.addSystemChatMessage(mapLabel + " has been reset.");
    };
    table[Opcode::SMSG_INSTANCE_RESET_FAILED] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint32_t mapId  = packet.readUInt32();
        uint32_t reason = packet.readUInt32();
        static const char* resetFailReasons[] = {
            "Not max level.", "Offline party members.", "Party members inside.",
            "Party members changing zone.", "Heroic difficulty only."
        };
        const char* reasonMsg = (reason < 5) ? resetFailReasons[reason] : "Unknown reason.";
        std::string mapLabel = queries_.mapName(mapId);
        if (mapLabel.empty()) mapLabel = "instance #" + std::to_string(mapId);
        raiseUiError("Cannot reset " + mapLabel + ": " + reasonMsg);
        chat_.addSystemChatMessage("Cannot reset " + mapLabel + ": " + reasonMsg);
    };
    table[Opcode::SMSG_INSTANCE_LOCK_WARNING_QUERY] = [this](network::Packet& packet) {
        // Nine bytes: a countdown in milliseconds, the mask of encounters
        // already completed in this instance, and whether the player was
        // previously saved to it.
        //
        // What was here read a map id, a difficulty, a time and a flag across
        // seventeen bytes, so the guard alone kept the handler from ever
        // running against the nine the server sends. It also answered on the
        // player's behalf - always accept - which is the one thing this packet
        // is asking them.
        if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
        const uint32_t timerMs = packet.readUInt32();
        const uint32_t completedMask = packet.readUInt32();
        const uint8_t previouslySaved = packet.readUInt8();

        instanceLock_.active = true;
        instanceLock_.secondsLeft = static_cast<float>(timerMs) / 1000.0f;
        instanceLock_.previouslySaved = (previouslySaved != 0);
        instanceLock_.completedEncounterMask = completedMask;

        LOG_INFO("SMSG_INSTANCE_LOCK_WARNING_QUERY: ", timerMs / 1000,
                 "s to decide, completed mask 0x", std::hex, completedMask, std::dec,
                 previouslySaved ? ", previously saved" : "");

        frameXml_.fireFrameXmlEvent("INSTANCE_LOCK_START", {});
    };

    // ---- LFG ----
    table[Opcode::SMSG_LFG_JOIN_RESULT] = [this](network::Packet& packet) { handleLfgJoinResult(packet); };
    table[Opcode::SMSG_LFG_QUEUE_STATUS] = [this](network::Packet& packet) { handleLfgQueueStatus(packet); };
    table[Opcode::SMSG_LFG_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_ROLE_CHECK_UPDATE] = [this](network::Packet& packet) { handleLfgRoleCheckUpdate(packet); };
    // Two opcodes, two bodies. They were sharing a handler, and the party one
    // carries a join flag and three more bytes of padding the player one does
    // not - so every party update read the wrong byte as the queue flag.
    table[Opcode::SMSG_LFG_UPDATE_PLAYER] = [this](network::Packet& packet) { handleLfgUpdatePlayer(packet); };
    table[Opcode::SMSG_LFG_UPDATE_PARTY]  = [this](network::Packet& packet) { handleLfgUpdateParty(packet); };
    table[Opcode::SMSG_LFG_PLAYER_REWARD] = [this](network::Packet& packet) { handleLfgPlayerReward(packet); };
    table[Opcode::SMSG_LFG_BOOT_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgBootProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_TELEPORT_DENIED] = [this](network::Packet& packet) { handleLfgTeleportDenied(packet); };
    table[Opcode::SMSG_LFG_DISABLED] = [this](network::Packet& /*packet*/) {
        if (auto* feed = lfgFeed(queries_)) feed->onDisabled();
    };
    table[Opcode::SMSG_LFG_OFFER_CONTINUE] = [this](network::Packet& packet) {
        // uint32 dungeon entry, which the handler used to skip entirely - and
        // then fired the event with nothing. lfgframe.lua opens with
        // NORMAL_FONT_COLOR_CODE..displayName.."|r", and concatenating nil
        // raises, so the offer took its own handler down every time.
        //
        // The entry packs both halves the way LFGDungeonData::Entry builds it:
        // id in the low twenty-four bits, type in the top eight.
        uint32_t dungeonId = 0, typeId = 0;
        if (packet.hasRemaining(4)) {
            const uint32_t entry = packet.readUInt32();
            dungeonId = entry & 0x00FFFFFFu;
            typeId    = (entry >> 24) & 0xFFu;
        }
        if (auto* feed = lfgFeed(queries_)) feed->onContinueOffered(dungeonId, typeId);
    };
    table[Opcode::SMSG_LFG_ROLE_CHOSEN] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(13)) { packet.skipAll(); return; }
        const uint64_t roleGuid = packet.readUInt64();
        const bool ready = packet.readUInt8() != 0;
        const uint32_t roles = packet.readUInt32();
        packet.skipAll();
        if (auto* feed = lfgFeed(queries_)) feed->onRoleChosen(roleGuid, ready, roles);
    };
    // The one that carries the locks. Skipped wholesale until now, which is
    // why the dungeon finder listed every dungeon as queueable and left the
    // server to refuse.
    table[Opcode::SMSG_LFG_PLAYER_INFO] = [this](network::Packet& packet) {
        handleLfgPlayerInfo(packet);
    };
    for (auto op : { Opcode::SMSG_LFG_UPDATE_SEARCH, Opcode::SMSG_UPDATE_LFG_LIST,
                     Opcode::SMSG_LFG_PARTY_INFO }) {
        table[op] = [](network::Packet& packet) { packet.skipAll(); };
    }
    table[Opcode::SMSG_OPEN_LFG_DUNGEON_FINDER] = [this](network::Packet& packet) {
        packet.skipAll();
        events_.publish(events::DungeonFinderOpened{});
    };

    // ---- Arena ----
    // command(4) + team name + player name + error(4).
    //
    // The player name was never read, so the error came out of the first four
    // bytes of it - a name, so never zero, and every command result read as a
    // failure however well it had gone. Both strings are needed anyway: the
    // server names the team and the player separately because most of these
    // results are about one acting on the other.
    table[Opcode::SMSG_ARENA_TEAM_COMMAND_RESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(10)) return;
        const uint32_t command = packet.readUInt32();
        std::string teamName = packet.readString();
        std::string playerName = packet.readString();
        if (!packet.hasRemaining(4)) return;
        const uint32_t error = packet.readUInt32();
        auto* feed = arenaFeed(queries_);
        if (!feed) return;
        if (error == 0) feed->onCommandSucceeded(arenaCommand(command), std::move(teamName), std::move(playerName));
        else feed->onCommandRefused(arenaCommand(command), std::move(teamName), std::move(playerName),
                                    arenaCommandError(error));
    };
    table[Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const uint32_t teamId = packet.readUInt32();
        std::string teamName = packet.readString();
        uint32_t teamType = 0;
        if (packet.hasRemaining(4)) teamType = packet.readUInt32();
        if (auto* feed = arenaFeed(queries_)) feed->onTeamNamed(teamId, std::move(teamName), teamType);
    };
    table[Opcode::SMSG_ARENA_TEAM_ROSTER] = [this](network::Packet& packet) {
        auto roster = readArenaTeamRoster(packet);
        if (!roster) return;
        if (auto* feed = arenaFeed(queries_)) feed->onRoster(std::move(*roster));
    };
    table[Opcode::SMSG_ARENA_TEAM_INVITE] = [this](network::Packet& packet) {
        std::string inviter = packet.readString();
        std::string teamName = packet.readString();
        if (auto* feed = arenaFeed(queries_)) feed->onInvited(std::move(inviter), std::move(teamName));
    };
    table[Opcode::SMSG_ARENA_TEAM_EVENT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        const uint8_t event = packet.readUInt8();
        uint8_t strCount = 0;
        if (packet.hasRemaining(1)) strCount = packet.readUInt8();
        std::string first, second;
        if (strCount >= 1 && packet.getSize() > packet.getReadPos()) first = packet.readString();
        if (strCount >= 2 && packet.getSize() > packet.getReadPos()) second = packet.readString();
        if (auto* feed = arenaFeed(queries_))
            feed->onTeamChanged(arenaTeamChange(event), std::move(first), std::move(second), event);
    };
    table[Opcode::SMSG_ARENA_TEAM_STATS] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(28)) return;
        arena::ArenaTeamStats stats;
        stats.teamId = packet.readUInt32(); stats.rating = packet.readUInt32();
        stats.weekGames = packet.readUInt32(); stats.weekWins = packet.readUInt32();
        stats.seasonGames = packet.readUInt32(); stats.seasonWins = packet.readUInt32();
        stats.rank = packet.readUInt32();
        if (auto* feed = arenaFeed(queries_)) feed->onStats(std::move(stats));
    };
    table[Opcode::SMSG_ARENA_ERROR] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const uint32_t error = packet.readUInt32();
        if (auto* feed = arenaFeed(queries_)) feed->onMatchRefused(arenaMatchError(error), error);
    };
    table[Opcode::MSG_PVP_LOG_DATA] = [this](network::Packet& packet) {
        auto board = readPvpLog(packet, queries_.entities());
        if (!board) return;
        if (auto* feed = battlegroundFeed(queries_)) feed->onScoreboard(std::move(*board));
    };

    // ---- Factions / group leader ----
}

// ============================================================
// Non-inline accessors requiring GameHandler
// ============================================================

uint32_t SocialHandler::ownGuildId() const {
    // The player's own field first: it is what the server keeps current, and
    // the character list's copy was taken at login - before any guild joined
    // or left since.
    if (const uint32_t live = getEntityGuildId(queries_.characterGuid())) return live;
    const Character* ch = queries_.activeCharacter();
    return ch ? ch->guildId : 0;
}

bool SocialHandler::isInGuild() const {
    // Three records, and the newest wins: a guild joined this session is in
    // none of the others until the next login.
    if (inGuild_) return true;
    if (!guildName_.empty()) return true;
    return ownGuildId() != 0;
}

uint32_t SocialHandler::getEntityGuildId(uint64_t guid) const {
    auto entity = queries_.entities().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) return 0;
    const uint16_t ufUnitEnd = fieldIndex(UF::UNIT_END);
    if (ufUnitEnd == 0xFFFF) return 0;
    return entity->getField(ufUnitEnd + 3);
}

const std::string& SocialHandler::lookupGuildName(uint32_t guildId) {
    if (guildId == 0) return kEmptyString;
    auto it = guildNameCache_.find(guildId);
    if (it != guildNameCache_.end()) return it->second;

    const auto now = std::chrono::steady_clock::now();
    auto pendingIt = pendingGuildNameQueries_.find(guildId);
    const bool shouldQuery =
        pendingIt == pendingGuildNameQueries_.end() ||
        std::chrono::duration_cast<std::chrono::seconds>(now - pendingIt->second).count() >= 2;
    if (shouldQuery) {
        pendingGuildNameQueries_[guildId] = now;
        queryGuildInfo(guildId);
    }
    return kEmptyString;
}

// ============================================================
// Inspection
// ============================================================

void SocialHandler::inspectTarget() { inspectUnit(queries_.currentTargetGuid()); }

// Any player, not only the targeted one: FrameXML's unit menus name the unit
// the menu was opened on, and on a party frame that is not the target.
void SocialHandler::requestInspectHonorData(uint64_t guid) {
    if (guid == 0) return;
    if (!sender_.characterInWorld()) return;
    const uint32_t wire = wireOpcode(Opcode::MSG_INSPECT_HONOR_STATS);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    pkt.writeUInt64(guid);
    sender_.send(pkt);
}

void SocialHandler::inspectUnit(uint64_t guid) {
    if (!sender_.characterInWorld()) {
        LOG_WARNING("Cannot inspect: not in world or not connected");
        return;
    }
    if (guid == 0) {
        raiseUiError("You must target a player to inspect.");
        return;
    }
    auto entity = queries_.entities().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) {
        chat_.addSystemChatMessage("You can only inspect players.");
        return;
    }
    auto packet = InspectPacket::build(guid);
    sender_.send(packet);
    if (isActiveExpansion("wotlk")) {
        auto achPkt = QueryInspectAchievementsPacket::build(guid);
        sender_.send(achPkt);
    }
    auto player = std::static_pointer_cast<Player>(entity);
    std::string name = player->getName().empty() ? "Target" : player->getName();
    chat_.addSystemChatMessage("Inspecting " + name + "...");
    LOG_INFO("Sent inspect request for player: ", name, " (GUID: 0x", std::hex, guid, std::dec, ")");
}

void SocialHandler::handleInspectResults(network::Packet& packet) {
    if (isActiveExpansion("tbc") && packet.getOpcode() == wireOpcode(Opcode::SMSG_INSPECT_RESULTS_UPDATE)) {
        uint64_t guid = 0;
        std::array<uint32_t, 19> items{};
        std::array<uint16_t, 19> enchantIds{};
        if (parseInspectEquipmentPayload(packet, guid, items, enchantIds)) {
            queries_.cacheInspectedPlayerEquipment(guid, items, enchantIds);

            auto entity = queries_.entities().getEntity(guid);
            std::string playerName = "Target";
            if (entity) {
                auto player = std::dynamic_pointer_cast<Player>(entity);
                if (player && !player->getName().empty()) playerName = player->getName();
            }

            // TBC servers send inspected gear separately from talent data. Make the
            // gear-only response visible to the Inspect window immediately while
            // preserving talents if a matching talent response already arrived.
            if (inspectResult_.guid != guid) {
                inspectResult_ = InspectResult{};
                inspectResult_.guid = guid;
            }
            inspectResult_.playerName = playerName;
            inspectResult_.itemEntries = items;
            inspectResult_.enchantIds = enchantIds;

            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE (TBC gear): ", playerName, " has gear in ",
                     std::count_if(items.begin(), items.end(),
                                   [](uint32_t e) { return e != 0; }), "/19 slots");
            if (frameXml_.frameXmlListening()) {
                char guidBuf[32];
                snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
                frameXml_.fireFrameXmlEvent("INSPECT_READY", {guidBuf});
                // INSPECT_READY is a later expansion's name and nothing in this
                // interface listens for it. The inspect paperdoll refreshes on
                // UNIT_INVENTORY_CHANGED for the unit it is showing, so the gear
                // arrived and the window was never told - it kept whatever it had
                // when it opened. Fired for the unit token this guid answers to,
                // and only when it answers to one: an inspected player who is not
                // the target has no token for the interface to match against.
                const std::string inspectUnit = queries_.unitIdFor(guid);
                if (!inspectUnit.empty()) {
                    frameXml_.fireFrameXmlEvent("UNIT_INVENTORY_CHANGED", {inspectUnit});
                }
            }
            return;
        }
        LOG_DEBUG("SMSG_INSPECT_RESULTS_UPDATE (TBC gear): unrecognized payload size=",
                  packet.getSize());
    }

    if (isActiveExpansion("tbc") &&
        packet.getOpcode() == wireOpcode(Opcode::SMSG_INSPECT_TALENT)) {
        if (!packet.hasFullPackedGuid()) return;
        uint64_t guid = packet.readPackedGuid();
        if (guid == 0) return;

        auto entity = queries_.entities().getEntity(guid);
        std::string playerName = "Target";
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) playerName = player->getName();
        }

        if (inspectResult_.guid != guid) {
            inspectResult_ = InspectResult{};
            inspectResult_.guid = guid;
        }
        inspectResult_.playerName = playerName;

        uint32_t unspentTalents = 0;
        uint32_t spentTalents = 0;
        std::array<uint32_t, 3> treePoints{};
        bool hasTreePoints = false;
        const uint8_t classId = queries_.playerClassOf(guid);
        const size_t talentPayloadStart = packet.getReadPos();
        if (parseTbcInspectTalentPayload(packet, queries_, classId, unspentTalents, spentTalents,
                                         treePoints, hasTreePoints)) {
            inspectResult_.totalTalents = spentTalents;
            inspectResult_.unspentTalents = unspentTalents;
            inspectResult_.hasTalentData = true;
            inspectResult_.hasTalentTreePoints = hasTreePoints;
            inspectResult_.talentTreePoints = treePoints;
            inspectResult_.talentGroups = 1;
            inspectResult_.activeTalentGroup = 0;
        } else {
            packet.setReadPos(talentPayloadStart);
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): ", playerName,
                     " unparsed payload bytes=", packet.getRemainingSize());
        }

        const auto* inspected = queries_.inspectedItemEntries(guid);
        if (inspected) inspectResult_.itemEntries = *inspected;

        LOG_INFO("SMSG_INSPECT_TALENT (TBC): ", playerName,
                 " gearCached=", (inspected ? "yes" : "no"),
                 " payload bytes remaining=", packet.getRemainingSize());
        if (frameXml_.frameXmlListening()) {
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
            frameXml_.fireFrameXmlEvent("INSPECT_READY", {guidBuf});
            const std::string inspectUnit = queries_.unitIdFor(guid);
            if (!inspectUnit.empty()) {
                frameXml_.fireFrameXmlEvent("UNIT_INVENTORY_CHANGED", {inspectUnit});
            }
        }
        return;
    }

    // SMSG_INSPECT_TALENT (WotLK) is a packed guid, then
    // BuildPlayerTalentsInfoData and BuildEnchantmentsInfoData. There is no
    // leading discriminator, and reading one ate the packed guid's mask byte
    // and put everything after it a byte out.
    //
    // The branch that used to sit behind that byte re-parsed the player's own
    // talents into a second set of members on GameHandler. Nothing read them -
    // SpellHandler keeps the ones the talent frame draws from - and this
    // opcode never carries them anyway.
    const bool talentTbc = isPreWotlk();
    if (packet.getRemainingSize() < (talentTbc ? 8u : 2u)) return;
    uint64_t guid = talentTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (guid == 0) return;

    size_t bytesLeft = packet.getRemainingSize();
    if (bytesLeft < 6) {
        // Before WotLK the guid is the whole answer - a 1.12 realm sends eight
        // bytes and nothing after them, which is the shape and not a
        // truncation. Reported as one, every inspect on turtle wrote a warning
        // into a log that is read for faults.
        if (talentTbc) {
            LOG_INFO("Inspect: this realm answers with the guid alone, so there "
                     "are no talents to show");
        } else {
            LOG_WARNING("SMSG_TALENTS_INFO: too short after guid, ", bytesLeft, " bytes");
        }
        auto entity = queries_.entities().getEntity(guid);
        std::string name = "Target";
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) name = player->getName();
        }
        chat_.addSystemChatMessage("Inspecting " + name + " (no talent data available).");
        return;
    }

    uint32_t unspentTalents = packet.readUInt32();
    uint8_t talentGroupCount = packet.readUInt8();
    uint8_t activeTalentGroup = packet.readUInt8();

    auto entity = queries_.entities().getEntity(guid);
    std::string playerName = "Target";
    if (entity) {
        auto player = std::dynamic_pointer_cast<Player>(entity);
        if (player && !player->getName().empty()) playerName = player->getName();
    }

    // Every spec is sent. Counting all of them reported a dual-spec character
    // as having spent roughly twice what they have.
    uint32_t totalTalents = 0;
    std::unordered_map<uint32_t, uint8_t> inspectTalents;
    for (uint8_t g = 0; g < talentGroupCount && g < 2; ++g) {
        bytesLeft = packet.getRemainingSize();
        if (bytesLeft < 1) break;
        uint8_t talentCount = packet.readUInt8();
        for (uint8_t t = 0; t < talentCount; ++t) {
            bytesLeft = packet.getRemainingSize();
            if (bytesLeft < 5) break;
            const uint32_t talentId = packet.readUInt32();
            const uint8_t rank = packet.readUInt8();
            if (g == activeTalentGroup) {
                // Rank is zero-based on the wire, as it is for the player's own
                // talents. The inspect talent tab was counting these and
                // throwing them away, so it drew the viewer's own tree with the
                // target's name over it.
                inspectTalents[talentId] = static_cast<uint8_t>(rank + 1u);
                totalTalents++;
            }
        }
        bytesLeft = packet.getRemainingSize();
        if (bytesLeft < 1) break;
        uint8_t glyphCount = packet.readUInt8();
        for (uint8_t gl = 0; gl < glyphCount; ++gl) {
            bytesLeft = packet.getRemainingSize();
            if (bytesLeft < 2) break;
            packet.readUInt16();
        }
    }

    // BuildEnchantmentsInfoData: a mask of which of the nineteen equipment
    // slots are filled, then per filled slot the item's entry, a mask of which
    // of its twelve enchantment slots are used, and an id per set bit.
    //
    // What was here read the slot mask and then one bare uint16 per set bit,
    // so it took the low half of the first item's entry as that slot's enchant
    // and drifted further out of step with every item after it.
    constexpr int kEquipmentSlots = 19;
    constexpr int kEnchantmentSlots = 12;
    std::array<uint32_t, 19> itemEntries{};
    std::array<uint16_t, 19> enchantIds{};
    bool sawGear = false;
    if (packet.hasRemaining(4)) {
        const uint32_t slotMask = packet.readUInt32();
        for (int slot = 0; slot < kEquipmentSlots; ++slot) {
            if (!(slotMask & (1u << slot))) continue;
            if (!packet.hasRemaining(6)) break;
            itemEntries[slot] = packet.readUInt32();
            sawGear = true;
            const uint16_t enchantMask = packet.readUInt16();
            for (int e = 0; e < kEnchantmentSlots; ++e) {
                if (!(enchantMask & (1u << e))) continue;
                if (!packet.hasRemaining(2)) break;
                const uint16_t enchId = packet.readUInt16();
                // Slot zero is the permanent enchant, which is the one the
                // paperdoll names.
                if (e == 0) enchantIds[slot] = enchId;
            }
        }
    }

    // A different player means the honour figures in hand are somebody else's.
    // The fields here are assigned one at a time rather than as a whole struct,
    // so anything not named survives - and the honour tab reads
    // HasInspectHonorData to decide whether to ask, so a stale true would show
    // the last player's kills against this one's name and never correct itself.
    if (inspectResult_.guid != guid) {
        inspectResult_.hasHonorData = false;
        inspectResult_.honorTodayKills = 0;
        inspectResult_.honorYesterdayKills = 0;
        inspectResult_.honorTodayContribution = 0;
        inspectResult_.honorYesterdayContribution = 0;
        inspectResult_.honorLifetimeKills = 0;
        inspectResult_.honorRank = 0;
    }
    inspectResult_.guid              = guid;
    inspectResult_.playerName        = playerName;
    inspectResult_.totalTalents      = totalTalents;
    inspectResult_.unspentTalents    = unspentTalents;
    inspectResult_.hasTalentData     = true;
    inspectResult_.hasTalentTreePoints = false;
    inspectResult_.talentTreePoints  = {};
    inspectResult_.talentGroups      = talentGroupCount;
    inspectResult_.activeTalentGroup = activeTalentGroup;
    inspectResult_.enchantIds        = enchantIds;
    inspectResult_.talentRanks       = std::move(inspectTalents);
    inspectResult_.classId           = queries_.playerClassOf(guid);

    // The gear is in this packet. It used to be looked up from the cache that
    // Classic's SMSG_INSPECT and TBC's SMSG_INSPECT_RESULTS_UPDATE fill, and
    // nothing filled it on WotLK, so inspecting anyone showed an empty
    // paperdoll however well equipped they were.
    if (sawGear) {
        inspectResult_.itemEntries = itemEntries;
        queries_.cacheInspectedPlayerEquipment(guid, itemEntries, enchantIds);
    } else {
        const auto* inspected = queries_.inspectedItemEntries(guid);
        inspectResult_.itemEntries = inspected ? *inspected : std::array<uint32_t, 19>{};
    }

    LOG_INFO("Inspect results for ", playerName, ": ", totalTalents, " talents, ",
             unspentTalents, " unspent, ", (int)talentGroupCount, " specs");
    if (frameXml_.frameXmlListening()) {
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
        frameXml_.fireFrameXmlEvent("INSPECT_READY", {guidBuf});
        // The same packet carries the talents, and the talent tab of the
        // inspect window redraws on this event alone. Only INSPECT_READY was
        // fired, which is what the gear tab listens for - so inspecting anyone
        // filled the paperdoll and left the talent tab blank, however
        // completely the ranks above had been parsed.
        frameXml_.fireFrameXmlEvent("INSPECT_TALENT_READY", {});
        const std::string inspectUnit = queries_.unitIdFor(guid);
        if (!inspectUnit.empty()) {
            frameXml_.fireFrameXmlEvent("UNIT_INVENTORY_CHANGED", {inspectUnit});
        }
    }
}

// ============================================================
// Server Info / Who / Social
// ============================================================

void SocialHandler::queryServerTime(bool announce) {
    if (!sender_.characterInWorld()) return;
    announceServerTime_ = announce;
    auto packet = QueryTimePacket::build();
    sender_.send(packet);
    LOG_INFO("Requested server time");
}

void SocialHandler::requestPlayedTime() {
    if (!sender_.characterInWorld()) return;
    auto packet = RequestPlayedTimePacket::build(true);
    sender_.send(packet);
    LOG_INFO("Requested played time");
}

void SocialHandler::randomRoll(uint32_t minRoll, uint32_t maxRoll) {
    if (!sender_.characterInWorld()) return;
    if (minRoll > maxRoll) std::swap(minRoll, maxRoll);
    if (maxRoll > 10000) maxRoll = 10000;
    auto packet = RandomRollPacket::build(minRoll, maxRoll);
    sender_.send(packet);
    LOG_INFO("Rolled ", minRoll, "-", maxRoll);
}

// ============================================================
// Logout
// ============================================================

void SocialHandler::requestLogout(bool exitAfterLogout) {
    // Exit Game has to close the program. Everything below is about leaving the
    // world tidily first, which is worth doing and is not worth being trapped
    // by: each way the request cannot be made is answered by going anyway.
    //
    // With no connection there is nobody to ask. At the login screen, or after
    // a disconnect, the button did nothing whatsoever.
    if (!sender_.connected()) {
        if (exitAfterLogout) {
            events_.publish(events::ExitRequested{});
        }
        return;
    }
    if (loggingOut_) {
        // Asked a second time while a logout is already running. For Exit that
        // is the player saying they would rather not wait, which is exactly
        // what ForceQuit is described as doing and what the countdown popup's
        // own button is for.
        if (exitAfterLogout) {
            LOG_WARNING("Exit Game asked for during a logout - closing now");
            events_.publish(events::ExitRequested{});
            return;
        }
        chat_.addSystemChatMessage("Already logging out.");
        return;
    }
    auto packet = LogoutRequestPacket::build();
    sender_.send(packet);
    loggingOut_ = true;
    exitAfterLogout_ = exitAfterLogout;
    LOG_WARNING("Sent logout request (exitAfterLogout=", exitAfterLogout ? "yes" : "no", ")");  // temporary
}

void SocialHandler::cancelLogout() {
    if (!sender_.connected()) return;
    if (!loggingOut_) { chat_.addSystemChatMessage("Not currently logging out."); return; }
    auto packet = LogoutCancelPacket::build();
    sender_.send(packet);
    loggingOut_ = false;
    exitAfterLogout_ = false;
    logoutCountdown_ = 0.0f;
    logoutCommitted_ = false;
    chat_.addSystemChatMessage("Logout cancelled.");
    LOG_INFO("Cancelled logout");
}

// ============================================================
// Guild
// ============================================================

void SocialHandler::requestGuildInfo() {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildInfoPacket::build();
    sender_.send(packet);
}

void SocialHandler::requestGuildRoster() {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildRosterPacket::build();
    sender_.send(packet);
    // Said in the log, not to the player. This is asked for far more often
    // than a person would ask for it: eight places here request a roster after
    // a guild change, and the interface asks whenever the mail frame or the
    // friends frame opens with an empty one. Every one of those put a line in
    // chat that the real client never writes.
    LOG_DEBUG("Requested guild roster");
}

// The longer "guild information" text, which is a different field from the
// message of the day and has its own opcode.
void SocialHandler::setGuildInfoText(const std::string& text) {
    if (!sender_.characterInWorld()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_INFO_TEXT));
    packet.writeString(text);
    sender_.send(packet);
    requestGuildRoster();
}

// Turn a letter into a readable item, which is what "take" does to a mail whose
// body is the thing being kept.
void SocialHandler::takeInboxTextItem(uint32_t mailId) {
    if (!sender_.characterInWorld()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_CREATE_TEXT_ITEM));
    packet.writeUInt64(0);            // mailbox guid - the server uses the open one
    packet.writeUInt32(mailId);
    sender_.send(packet);
}

void SocialHandler::setGuildMotd(const std::string& motd) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildMotdPacket::build(motd);
    sender_.send(packet);
    chat_.addSystemChatMessage("Guild MOTD updated.");
}

void SocialHandler::promoteGuildMember(const std::string& playerName) {
    if (!sender_.characterInWorld()) return;
    if (playerName.empty()) { raiseUiError("You must specify a player name."); return; }
    auto packet = GuildPromotePacket::build(playerName);
    sender_.send(packet);
    chat_.addSystemChatMessage("Promoting " + playerName + "...");
}

void SocialHandler::demoteGuildMember(const std::string& playerName) {
    if (!sender_.characterInWorld()) return;
    if (playerName.empty()) { raiseUiError("You must specify a player name."); return; }
    auto packet = GuildDemotePacket::build(playerName);
    sender_.send(packet);
    chat_.addSystemChatMessage("Demoting " + playerName + "...");
}

void SocialHandler::leaveGuild() {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildLeavePacket::build();
    sender_.send(packet);
    chat_.addSystemChatMessage("Leaving guild...");
}

void SocialHandler::inviteToGuild(const std::string& playerName) {
    if (!sender_.characterInWorld()) return;
    if (playerName.empty()) { raiseUiError("You must specify a player name."); return; }
    auto packet = GuildInvitePacket::build(playerName);
    sender_.send(packet);
    chat_.addSystemChatMessage("Inviting " + playerName + " to guild...");
}

void SocialHandler::kickGuildMember(const std::string& playerName) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildRemovePacket::build(playerName);
    sender_.send(packet);
}

void SocialHandler::disbandGuild() {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildDisbandPacket::build();
    sender_.send(packet);
}

void SocialHandler::setGuildLeader(const std::string& name) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildLeaderPacket::build(name);
    sender_.send(packet);
}

void SocialHandler::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildSetPublicNotePacket::build(name, note);
    sender_.send(packet);
}

void SocialHandler::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildSetOfficerNotePacket::build(name, note);
    sender_.send(packet);
}

void SocialHandler::acceptGuildInvite() {
    if (!sender_.characterInWorld()) return;
    pendingGuildInvite_ = false;
    // What the invite called it, until the guild names itself: the roster that
    // confirms the join does not carry a name, and the query that does needs
    // an id the player only has once the server has moved them.
    joinedGuildName_ = pendingGuildInviteGuildName_;
    auto packet = GuildAcceptPacket::build();
    sender_.send(packet);
    // ...and the roster, which is what fills the social window's guild tab.
    // The server sends the join event to the guild rather than a roster to the
    // joiner, so nothing arrived unless something asked.
    requestGuildRoster();
}

void SocialHandler::declineGuildInvite() {
    if (!sender_.characterInWorld()) return;
    pendingGuildInvite_ = false;
    auto packet = GuildDeclineInvitationPacket::build();
    sender_.send(packet);
}

void SocialHandler::queryGuildInfo(uint32_t guildId) {
    // Allow guild queries at the character screen too - the socket is
    // connected and the server accepts CMSG_GUILD_QUERY before login.
    if (!sender_.connected()) return;
    auto packet = GuildQueryPacket::build(guildId);
    sender_.send(packet);
}

void SocialHandler::createGuild(const std::string& guildName) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildCreatePacket::build(guildName);
    sender_.send(packet);
}

void SocialHandler::addGuildRank(const std::string& rankName) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildAddRankPacket::build(rankName);
    sender_.send(packet);
    requestGuildRoster();
}

void SocialHandler::deleteGuildRank() {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildDelRankPacket::build();
    sender_.send(packet);
    requestGuildRoster();
}

// ============================================================
// Reports
// ============================================================

void SocialHandler::reportPlayer(uint64_t targetGuid, const std::string& reason) {
    if (!sender_.characterInWorld()) return;
    if (targetGuid == 0) {
        raiseUiError("You must target a player to report.");
        return;
    }
    auto packet = ComplainPacket::build(targetGuid, reason);
    sender_.send(packet);
    chat_.addSystemChatMessage("Player report submitted.");
    LOG_INFO("Reported player: 0x", std::hex, targetGuid, std::dec, " reason=", reason);
}

void SocialHandler::reportMailSpam(uint64_t senderGuid, uint32_t mailId) {
    if (senderGuid == 0) return;
    if (!sender_.characterInWorld()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_COMPLAIN));
    packet.writeUInt8(0);              // 0 = mail, 1 = chat
    packet.writeUInt64(senderGuid);
    packet.writeUInt32(0);
    packet.writeUInt32(mailId);
    packet.writeUInt32(0);
    sender_.send(packet);
}

void SocialHandler::setGuildBankTabText(uint8_t tab, const std::string& text) {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildBankSetTabTextPacket::build(tab, text);
    sender_.send(packet);
}

void SocialHandler::channelModeration(Opcode op, const std::string& channelName,
                                     const std::string& targetName,
                                     bool allowEmptyTarget) {
    if (!sender_.characterInWorld()) return;
    // An empty second string is meaningless for the moderation commands - there
    // is no player called nothing - but it is how a channel password is
    // cleared, so that one caller says it means it.
    if (channelName.empty() || (targetName.empty() && !allowEmptyTarget)) return;
    auto packet = ChannelModerationPacket::build(op, channelName, targetName);
    sender_.send(packet);
}

// Removes the lowest rank, which is what the server does with this - it takes
// no index, so there is nothing to get wrong about which one goes.
void SocialHandler::delGuildRank() {
    if (!sender_.characterInWorld()) return;
    auto packet = GuildDelRankPacket::build();
    sender_.send(packet);
    requestGuildRoster();
}

void SocialHandler::setSavedInstanceExtend(uint32_t mapId, uint32_t difficulty, bool extend) {
    if (!sender_.characterInWorld()) return;
    // uint32 mapId, uint32 difficulty, uint8 toggle - HandleSetSavedInstanceExtend.
    // The server ignores it unless the bind is permanent and the flag actually
    // changes, so there is nothing to check here that it does not check better.
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_SAVED_INSTANCE_EXTEND));
    packet.writeUInt32(mapId);
    packet.writeUInt32(difficulty);
    packet.writeUInt8(extend ? 1 : 0);
    sender_.send(packet);
}

void SocialHandler::requestRaidInfo() {
    if (!sender_.characterInWorld()) return;
    auto packet = RequestRaidInfoPacket::build();
    sender_.send(packet);
    // Not said in chat. The interface asks for this at every login - it is what
    // fills the Raid Info panel - so the line was three words of noise in the
    // first frame of every session, describing a request nobody made. The
    // player-facing answer belongs to whoever asked: /raidinfo prints what came
    // back, and the panel draws it.
    LOG_INFO("CMSG_REQUEST_RAID_INFO: asked for saved instance lockouts");
}

// ============================================================
// Group Handlers
// ============================================================

void SocialHandler::handleGroupInvite(network::Packet& packet) {
    GroupInviteResponseData data;
    const bool hasCanAccept = !isPreWotlk();
    if (!GroupInviteResponseParser::parse(packet, data, hasCanAccept)) return;
    // The leading byte is not decoration: AzerothCore sends this same opcode
    // with a zero in it to say "you were invited and it could not be offered,
    // because you are already in a group" - its own comment beside the write,
    // and the interface carries Blizzard's sentence for exactly that case
    // (ERR_INVITED_ALREADY_IN_GROUP_SS).
    if (auto* feed = groupFeed(queries_)) feed->onInvite(data.inviterName, data.canAccept);
}

void SocialHandler::handleGroupList(network::Packet& packet) {
    const bool hasRoles = isActiveExpansion("wotlk");
    const bool hasBattleGroupFlag = isActiveExpansion("tbc");
    GroupListData roster;
    if (!GroupListParser::parse(packet, roster, hasRoles, hasBattleGroupFlag)) {
        LOG_WARNING("SMSG_GROUP_LIST: could not be read; the group stays as it was");
        return;
    }
    if (auto* feed = groupFeed(queries_)) feed->onRoster(std::move(roster));
}

void SocialHandler::handlePartyMemberStats(network::Packet& packet, bool isFull) {
    std::optional<std::vector<AuraSlot>> auras;
    const auto stats = readPartyMemberStats(packet, isFull, queries_, auras);
    if (!stats) return;
    // A unit that is not a member is not this packet's business; its auras are
    // left alone with the rest.
    const group::IGroup* groupView = queries_.group();
    bool isMember = false;
    if (groupView)
        for (const auto& m : groupView->getPartyData().members)
            if (m.guid == stats->guid) { isMember = true; break; }
    if (!isMember) { packet.skipAll(); return; }
    // Into the cache the party frames read through getUnitAuras, which is the
    // unit's state rather than the group's.
    if (auras && stats->guid != 0 && stats->guid != queries_.characterGuid() &&
        stats->guid != queries_.currentTargetGuid()) {
        queries_.mirrorAurasByGuid(stats->guid, *auras);
    }
    if (auto* feed = groupFeed(queries_)) feed->onMemberStats(*stats);
}

// ============================================================
// Guild Handlers
// ============================================================

void SocialHandler::handleGuildInfo(network::Packet& packet) {
    GuildInfoData data;
    if (!GuildInfoParser::parse(packet, data)) return;
    if (!data.guildName.empty()) {
        if (const Character* ch = queries_.activeCharacter(); ch && ch->hasGuild()) {
            rememberGuildName(ch->guildId, data.guildName);
            if (guildName_.empty()) guildName_ = data.guildName;
        }
    }
    // SMSG_GUILD_INFO is pushed by the server on every guild roster sync
    // (login, member join/leave, periodic refreshes). Only print when
    // something the user can see has actually changed; otherwise the
    // guild banner spams chat on every tick.
    const bool changed = (guildInfoData_.guildName != data.guildName) ||
                         (guildInfoData_.numMembers != data.numMembers) ||
                         (guildInfoData_.numAccounts != data.numAccounts);
    guildInfoData_ = data;
    if (changed) {
        chat_.addSystemChatMessage("Guild: " + data.guildName + " (" +
                             std::to_string(data.numMembers) + " members, " +
                             std::to_string(data.numAccounts) + " accounts)");
    }
}

void SocialHandler::handleGuildRoster(network::Packet& packet) {
    GuildRosterData data;
    if (!queries_.packetParsers().parseGuildRoster(packet, data)) return;
    guildRoster_ = std::move(data);
    hasGuildRoster_ = true;

    // A roster is only sent to a member, so this is the server saying so.
    //
    // Joining mid-session left every record of membership untouched: the
    // character list's guild id was taken at login and the guild name is only
    // written for a character that already had one. So the social window's
    // guild tab stayed empty for a player who had just accepted an invite, and
    // stayed empty until the next login.
    const bool newlyIn = !inGuild_;
    inGuild_ = true;
    if (guildName_.empty() && !joinedGuildName_.empty()) {
        guildName_ = joinedGuildName_;
    }
    if (guildName_.empty()) {
        // The name comes from the guild's own query, which needs the id - and
        // the id is on the player now that the server has moved them.
        if (const uint32_t id = ownGuildId()) queryGuildInfo(id);
    }
    if (newlyIn && frameXml_.frameXmlListening()) {
        // What the social window listens for to rebuild its guild tab. Without
        // it the frame has a roster and no idea it is meant to draw one.
        frameXml_.fireFrameXmlEvent("PLAYER_GUILD_UPDATE", {});
    }
    // False: the roster in hand is the fresh one, so nothing should ask again.
    // The argument is "you may request a roster", and every reader answers it
    // with `if arg1 then GuildRoster() end` - friendsframe, the guild bank and
    // the calendar all do exactly that. Sending true here would have each of
    // them request a roster, whose reply would fire this again, forever.
    if (frameXml_.frameXmlListening())
        frameXml_.fireFrameXmlEvent("GUILD_ROSTER_UPDATE", {eventBool(false)});
}

// CMSG_GUILD_RANK rewrites the rank whole: id, rights, name, gold per day, then
// six pairs of bank tab rights and slots. Everything it carries has to be sent,
// which is why the caller stages from the rank's current values rather than
// from nothing - a field left at zero here is not "unchanged", it is revoked.
void SocialHandler::saveGuildRank(uint32_t rankId, uint32_t rights, const std::string& rankName,
                                  uint32_t goldLimit, const uint32_t* tabRights,
                                  const uint32_t* tabSlots) {
    if (!sender_.characterInWorld()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_RANK));
    packet.writeUInt32(rankId);
    packet.writeUInt32(rights);
    packet.writeString(rankName);
    packet.writeUInt32(goldLimit);
    for (int t = 0; t < 6; ++t) {
        packet.writeUInt32(tabRights ? tabRights[t] : 0);
        packet.writeUInt32(tabSlots ? tabSlots[t] : 0);
    }
    sender_.send(packet);
    requestGuildRoster();
}

uint32_t SocialHandler::getPlayerGuildRankIndex() const {
    if (!hasGuildRoster_) return 0xFFFFFFFFu;
    const uint64_t me = queries_.characterGuid();
    for (const auto& m : guildRoster_.members)
        if (m.guid == me) return m.rankIndex;
    return 0xFFFFFFFFu;
}

uint32_t SocialHandler::getPlayerGuildRankRights() const {
    const uint32_t idx = getPlayerGuildRankIndex();
    if (idx >= guildRoster_.ranks.size()) return 0;
    return guildRoster_.ranks[idx].rights;
}

void SocialHandler::handleGuildQueryResponse(network::Packet& packet) {
    GuildQueryResponseData data;
    if (!queries_.packetParsers().parseGuildQueryResponse(packet, data)) return;
    if (data.guildId != 0) {
        pendingGuildNameQueries_.erase(data.guildId);
        if (!data.guildName.empty()) {
            rememberGuildName(data.guildId, data.guildName);
        }
    }
    // Ours when it names the guild this character is actually in - which is
    // the player's own field once they have joined one, not the character
    // list's copy from login.
    const bool isLocalGuild = (ownGuildId() != 0 && ownGuildId() == data.guildId);
    if (isLocalGuild) {
        const bool wasUnknown = guildName_.empty();
        guildName_ = data.guildName;
        guildQueryData_ = data;
        guildRankNames_.clear();
        for (const auto& rankName : data.rankNames) guildRankNames_.push_back(rankName);
        if (wasUnknown && !guildName_.empty()) {
            chat_.addSystemChatMessage("Guild: <" + guildName_ + ">");
            if (frameXml_.frameXmlListening()) frameXml_.fireFrameXmlEvent("PLAYER_GUILD_UPDATE", {});
        }
    }
}

// Returns true only when the member's cached online state actually changes (and updates
// it), so callers announce genuine online/offline transitions and silently drop the
// redundant SIGNED_ON flood the server emits at login for already-online members.
bool SocialHandler::guildMemberOnlineTransition(const std::string& name, bool nowOnline) {
    for (auto& m : guildRoster_.members) {
        if (m.name == name) {
            if (m.online == nowOnline) return false;
            m.online = nowOnline;
            return true;
        }
    }
    return true;  // not in the cached roster yet - announce rather than swallow
}

void SocialHandler::handleGuildEvent(network::Packet& packet) {
    GuildEventData data;
    if (!GuildEventParser::parse(packet, data)) return;

    // What the game prints for each, in its own words (ERR_GUILD_* and the
    // friend list's online and offline lines) and as a system line. These went
    // out as guild chat with no sender, and the interface puts the player's own
    // name on a local line without one: "[Guild] [You]: [Guild] Coggs has come
    // online."
    std::string msg;
    switch (data.eventType) {
        case GuildEvent::PROMOTION:
            if (data.numStrings >= 3)
                msg = data.strings[0] + " has promoted " + data.strings[1] + " to " + data.strings[2] + ".";
            break;
        case GuildEvent::DEMOTION:
            if (data.numStrings >= 3)
                msg = data.strings[0] + " has demoted " + data.strings[1] + " to " + data.strings[2] + ".";
            break;
        case GuildEvent::MOTD:
            // Not a line from here: chatframe.lua formats GUILD_MOTD_TEMPLATE
            // from the GUILD_MOTD event fired further down, and the chat
            // window this client drew for itself is gone.
            break;
        case GuildEvent::JOINED:
            if (data.numStrings >= 1) msg = data.strings[0] + " has joined the guild.";
            break;
        case GuildEvent::LEFT:
            if (data.numStrings >= 1) msg = data.strings[0] + " has left the guild.";
            break;
        case GuildEvent::REMOVED:
            // The member removed, then who removed them - the order the server
            // sends and ERR_GUILD_REMOVE_SS reads them in.
            if (data.numStrings >= 2) msg = data.strings[0] + " has been kicked out of the guild by " + data.strings[1] + ".";
            break;
        case GuildEvent::LEADER_IS:
            if (data.numStrings >= 1) msg = data.strings[0] + " is the leader of your guild.";
            break;
        case GuildEvent::LEADER_CHANGED:
            if (data.numStrings >= 2) msg = data.strings[0] + " has made " + data.strings[1] + " the new Guild Master.";
            break;
        case GuildEvent::DISBANDED:
            msg = "Guild has been disbanded.";
            guildName_.clear();
            guildRankNames_.clear();
            guildRoster_ = GuildRosterData{};
            hasGuildRoster_ = false;
            if (frameXml_.frameXmlListening()) frameXml_.fireFrameXmlEvent("PLAYER_GUILD_UPDATE", {});
            break;
        case GuildEvent::SIGNED_ON:
            // Only announce a real offline→online transition. On login the server floods
            // SIGNED_ON for members the roster already lists as online; suppressing those
            // (state unchanged) matches the real client and stops the guild-chat spam.
            //
            // ...and only when the player wants to hear about it at all. Guild
            // Member Notify is offered by the panel and was read by nothing, so
            // a large guild announced every arrival and departure whatever it
            // said. The transition is still tracked when the message is
            // suppressed - the roster's idea of who is online is not the
            // player's idea of what is worth reading.
            if (data.numStrings >= 1 && guildMemberOnlineTransition(data.strings[0], true) &&
                cvars_.get("guildMemberNotify", "1") != "0")
                msg = "|Hplayer:" + data.strings[0] + "|h[" + data.strings[0] + "]|h has come online.";
            break;
        case GuildEvent::SIGNED_OFF:
            if (data.numStrings >= 1 && guildMemberOnlineTransition(data.strings[0], false) &&
                cvars_.get("guildMemberNotify", "1") != "0")
                msg = data.strings[0] + " has gone offline.";
            break;
        // The bank's half of the event list. The server broadcasts these to
        // every member whenever anyone touches the bank, so they are frequent,
        // and not one of them is a chat line - they carry the figures the bank
        // panel reads back through its own API. Unhandled, a guild with an
        // active bank filled guild chat with "Guild event 17: 000000000051451B".
        case GuildEvent::BANK_MONEY_SET:
        case GuildEvent::BANK_TAB_AND_MONEY:
            if (data.numStrings >= 1)
                queries_.setGuildBankMoney(guildEventMoney(data.strings[0]));
            break;
        case GuildEvent::BANK_BAG_SLOTS_CHANGED:
        case GuildEvent::BANK_TAB_PURCHASED:
        case GuildEvent::BANK_TAB_UPDATED:
        case GuildEvent::BANK_TEXT_CHANGED:
        case GuildEvent::TABARD_CHANGED:
        case GuildEvent::RANK_UPDATED:
        case GuildEvent::RANK_DELETED:
            // Silent here; the interface events they mean are fired below.
            break;
        default: {
            // Not a chat line either: an event this client does not know is a
            // gap in this switch, and guild chat is not where that belongs.
            // Once per type - these arrive as often as the bank ones do, and a
            // log that repeats itself answers nothing a single line does not.
            static bool warned[256] = {};
            if (!warned[data.eventType]) {
                warned[data.eventType] = true;
                LOG_WARNING("SMSG_GUILD_EVENT: unhandled type ", static_cast<int>(data.eventType),
                            " strings=", static_cast<int>(data.numStrings),
                            data.numStrings >= 1 ? (" first=" + data.strings[0]) : std::string());
            }
            break;
        }
    }

    if (!msg.empty()) {
        MessageChatData chatMsg;
        chatMsg.type = ChatType::SYSTEM;
        chatMsg.language = ChatLanguage::UNIVERSAL;
        chatMsg.message = msg;
        queries_.addLocalChatMessage(chatMsg);
    }

    // Whether this client is about to ask for a fresh roster itself, which is
    // the switch at the end of this function. The event's argument is the
    // negation: "nobody has asked, so you may". Working it out once and using
    // it in both places is the point - FrameXML requesting when the client
    // already has costs a second roster for a large guild, and FrameXML not
    // requesting when the client has not leaves every guild panel drawing from
    // a copy that no longer says who is online.
    const bool clientWillRequest = hasGuildRoster_ && (
        data.eventType == GuildEvent::PROMOTION ||
        data.eventType == GuildEvent::DEMOTION ||
        data.eventType == GuildEvent::JOINED ||
        data.eventType == GuildEvent::LEFT ||
        data.eventType == GuildEvent::REMOVED ||
        data.eventType == GuildEvent::LEADER_CHANGED ||
        // A rank added, renamed or deleted only reaches the client in the
        // roster: SMSG_GUILD_ROSTER is what carries the rank list, and every
        // panel that names a rank reads it from there.
        data.eventType == GuildEvent::RANK_UPDATED ||
        data.eventType == GuildEvent::RANK_DELETED);

    if (frameXml_.frameXmlListening()) {
        switch (data.eventType) {
            case GuildEvent::MOTD:
                frameXml_.fireFrameXmlEvent("GUILD_MOTD", {data.numStrings >= 1 ? data.strings[0] : ""});
                break;
            case GuildEvent::SIGNED_ON: case GuildEvent::SIGNED_OFF:
            case GuildEvent::PROMOTION: case GuildEvent::DEMOTION:
            case GuildEvent::JOINED: case GuildEvent::LEFT:
            case GuildEvent::REMOVED: case GuildEvent::LEADER_CHANGED:
            case GuildEvent::DISBANDED:
            case GuildEvent::RANK_UPDATED: case GuildEvent::RANK_DELETED:
                // Somebody joined, left, was promoted or signed on, and the
                // roster this client holds no longer says so. The server sends
                // the event and not a new roster, so the request has to come
                // from somewhere. Signing on and off is the case nothing
                // covered: the switch below does not request for those, so a
                // member who had just come online stayed grey until something
                // else asked.
                frameXml_.fireFrameXmlEvent("GUILD_ROSTER_UPDATE",
                                               {eventBool(!clientWillRequest)});
                break;
            // The bank panel's own events, which are what these types mean.
            // GuildBankFrame_OnEvent returns early unless the frame is
            // visible, so firing them while it is shut costs nothing.
            case GuildEvent::TABARD_CHANGED:
                frameXml_.fireFrameXmlEvent("GUILDTABARD_UPDATE", {});
                break;
            case GuildEvent::BANK_BAG_SLOTS_CHANGED:
                frameXml_.fireFrameXmlEvent("GUILDBANKBAGSLOTS_CHANGED", {});
                break;
            case GuildEvent::BANK_TAB_PURCHASED:
            case GuildEvent::BANK_TAB_UPDATED:
            case GuildEvent::BANK_TAB_AND_MONEY:
                // The money half of 18 went through setGuildBankMoney above,
                // which fires GUILDBANK_UPDATE_MONEY when the figure moved.
                frameXml_.fireFrameXmlEvent("GUILDBANK_UPDATE_TABS", {});
                break;
            case GuildEvent::BANK_TEXT_CHANGED:
                // The tab, as the server counts them from zero and the panel
                // counts them from one. It answers by querying the new text.
                if (data.numStrings >= 1)
                    frameXml_.fireFrameXmlEvent(
                        "GUILDBANK_TEXT_CHANGED",
                        {std::to_string(std::strtol(data.strings[0].c_str(), nullptr, 10) + 1)});
                break;
            default: break;
        }
    }

    // A new tabard is in the guild query, not the roster, so it is the query
    // that has to be asked again for the emblem to change.
    if (data.eventType == GuildEvent::TABARD_CHANGED) {
        if (const uint32_t id = ownGuildId()) queryGuildInfo(id);
    }

    if (clientWillRequest) requestGuildRoster();
}

void SocialHandler::handleGuildInvite(network::Packet& packet) {
    GuildInviteResponseData data;
    if (!GuildInviteResponseParser::parse(packet, data)) return;
    pendingGuildInvite_ = true;
    pendingGuildInviterName_ = data.inviterName;
    pendingGuildInviteGuildName_ = data.guildName;
    chat_.addSystemChatMessage(data.inviterName + " has invited you to join " + data.guildName + ".");
    if (frameXml_.frameXmlListening())
        frameXml_.fireFrameXmlEvent("GUILD_INVITE_REQUEST", {data.inviterName, data.guildName});
}

void SocialHandler::handleGuildCommandResult(network::Packet& packet) {
    GuildCommandResultData data;
    if (!GuildCommandResultParser::parse(packet, data)) return;
    if (data.errorCode == 0) {
        // Whether the player's own membership changed, which is a different
        // question from whether the command worked: inviting someone succeeds
        // without altering anything the panels read.
        bool membershipChanged = false;
        switch (data.command) {
            case 0:
                chat_.addSystemChatMessage("Guild created.");
                membershipChanged = true;
                break;
            case 1:
                if (!data.name.empty()) chat_.addSystemChatMessage("You have invited " + data.name + " to the guild.");
                break;
            case 2:
                chat_.addSystemChatMessage("You have left the guild.");
                guildName_.clear(); guildRankNames_.clear();
                guildRoster_ = GuildRosterData{}; hasGuildRoster_ = false;
                membershipChanged = true;
                break;
            default: break;
        }
        // Leaving a guild emptied the name, the ranks and the whole roster and
        // told nobody, so the interface's guild tab kept showing the guild the
        // player had just left - every row of it, until something else
        // happened to fire a roster update. PLAYER_GUILD_UPDATE is what
        // friendsframe and the paperdoll re-check IsInGuild on.
        //
        // The roster event carries false, which is the established contract
        // here: every reader answers a true with `if arg1 then GuildRoster()
        // end`, and the reply to that would fire this again without end.
        if (membershipChanged && frameXml_.frameXmlListening()) {
            frameXml_.fireFrameXmlEvent("PLAYER_GUILD_UPDATE", {});
            frameXml_.fireFrameXmlEvent("GUILD_ROSTER_UPDATE", {eventBool(false)});
        }
        return;
    }
    const char* errStr = nullptr;
    switch (data.errorCode) {
        case 2:  errStr = "You are not in a guild."; break;
        case 4:  errStr = "No player named \"%s\" is online."; break;
        case 11: errStr = "\"%s\" is already in a guild."; break;
        case 13: errStr = "You are already in a guild."; break;
        case 14: errStr = "\"%s\" has already been invited to a guild."; break;
        case 16: case 17: errStr = "You are not the guild leader."; break;
        case 22: errStr = "That player is ignoring you."; break;
        default: break;
    }
    std::string errorMsg;
    if (errStr) {
        std::string fmt = errStr;
        auto pos = fmt.find("%s");
        if (pos != std::string::npos && !data.name.empty()) fmt.replace(pos, 2, data.name);
        else if (pos != std::string::npos) fmt.replace(pos, 2, "that player");
        errorMsg = fmt;
    } else {
        errorMsg = "Guild command failed";
        if (!data.name.empty()) errorMsg += " for " + data.name;
        errorMsg += " (error " + std::to_string(data.errorCode) + ")";
    }
    raiseUiError(errorMsg);
    chat_.addSystemChatMessage(errorMsg);
}

void SocialHandler::handlePetitionShowlist(network::Packet& packet) {
    PetitionShowlistData data;
    if (!PetitionShowlistParser::parse(packet, data)) return;
    if (auto* feed = petitionFeed(queries_)) feed->onVendor(toCharterOffer(data));
}

void SocialHandler::handlePetitionQueryResponse(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 12) return;
    /*uint32_t entry =*/ packet.readUInt32();
    uint64_t petGuid = packet.readUInt64();
    std::string guildName = packet.readString();
    /*std::string body =*/ packet.readString();

    // Three words the reply carries that used to be skipped along with the
    // rest of it. The first is how many signatures the charter needs - nothing
    // else on the wire says, so signaturesRequired sat at its default of nine
    // for every charter, and the counter under the signature list read "x / 9"
    // whatever the realm was configured for. CanSignPetition compares against
    // the same number, so the Sign button stayed live past the real
    // requirement.
    //
    // The third tells a guild charter from an arena one: AzerothCore writes
    // zero for a guild and the team size otherwise. GetPetitionInfo answered
    // "guild" for everything, which is the wrong heading and the wrong
    // confirmation on all three arena charters.
    uint32_t needed = 0, arenaType = 0;
    if (rem() >= 12) {
        needed = packet.readUInt32();
        /*uint32_t neededAgain =*/ packet.readUInt32();
        arenaType = packet.readUInt32();
    }
    packet.skipAll();
    if (auto* feed = petitionFeed(queries_))
        feed->onDetails(petGuid, std::move(guildName), needed, arenaType != 0);
}

void SocialHandler::handlePetitionShowSignatures(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 21) return;
    const uint64_t petitionGuid = packet.readUInt64();
    const uint64_t ownerGuid = packet.readUInt64();
    const uint32_t petEntry = packet.readUInt32();
    const uint8_t sigCount = packet.readUInt8();
    std::vector<petition::PetitionSignature> signatures;
    signatures.reserve(sigCount);
    for (uint8_t i = 0; i < sigCount; ++i) {
        if (rem() < 12) break;
        petition::PetitionSignature sig;
        sig.playerGuid = packet.readUInt64();
        /*uint32_t unk =*/ packet.readUInt32();
        signatures.push_back(sig);
    }
    if (auto* feed = petitionFeed(queries_))
        feed->onSignatures(petitionGuid, ownerGuid, petEntry, sigCount, std::move(signatures));
}

void SocialHandler::handlePetitionSignResults(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 20) return;
    const uint64_t petGuid = packet.readUInt64();
    const uint64_t playerGuid = packet.readUInt64();
    const uint32_t result = packet.readUInt32();
    auto* feed = petitionFeed(queries_);
    if (!feed) return;
    if (result == 0) feed->onSigned(petGuid, playerGuid);
    else feed->onSignRefused(signRefusal(result), result);
}

// ============================================================
// Server Info Handlers
// ============================================================

void SocialHandler::handleQueryTimeResponse(network::Packet& packet) {
    QueryTimeResponseData data;
    if (!QueryTimeResponseParser::parse(packet, data)) return;

    // The first word is the server's wall clock, to the second - what the
    // realm clock keeps time from.
    events_.publish(events::ServerTimeReceived{data.serverTime});

    // The second word is how long until the daily quests reset -
    // GetNextDailyQuestsResetTime() minus now, in AzerothCore's own terms. It
    // was parsed and dropped, and GetQuestResetTime answered the time until
    // local midnight instead, with a comment saying this was a realm setting
    // nothing sends. It is sent, in the reply to a packet this client already
    // knew how to ask for.
    dailyResetOffset_ = data.timeOffset;
    dailyResetReceivedAt_ = time(nullptr);

    // Only /time says it out loud. The login-time ask is for the reset figure
    // and would otherwise greet every player with the clock.
    if (!announceServerTime_) return;
    announceServerTime_ = false;
    time_t serverTime = static_cast<time_t>(data.serverTime);
    const std::tm timeInfo = core::localTime(serverTime);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeInfo);
    chat_.addSystemChatMessage("Server time: " + std::string(timeStr));
}

uint32_t SocialHandler::getSecondsUntilDailyReset() const {
    if (dailyResetOffset_ == 0 || dailyResetReceivedAt_ == 0) return 0;
    const auto elapsed = static_cast<int64_t>(time(nullptr) - dailyResetReceivedAt_);
    const int64_t left = static_cast<int64_t>(dailyResetOffset_) - elapsed;
    // Past the reset the answer is stale rather than negative; the next login
    // asks again. Zero reads as "not known", which is the honest answer.
    return left > 0 ? static_cast<uint32_t>(left) : 0;
}

void SocialHandler::handlePlayedTime(network::Packet& packet) {
    PlayedTimeData data;
    if (!PlayedTimeParser::parse(packet, data)) return;
    totalTimePlayed_ = data.totalTimePlayed;
    levelTimePlayed_ = data.levelTimePlayed;
    if (data.triggerMessage) {
        // TIME_PLAYED_MSG(totalTime, levelTime), in seconds, which is what
        // ChatFrame_SystemEventHandler dispatches to ChatFrame_DisplayTimePlayed
        // - the real client's own answer to /played, down to the localised
        // "days, hours, minutes, seconds" string. The server also sends played
        // time unasked at login, and triggerMessage is what tells the two
        // apart, so this stays inside it.
        frameXml_.fireFrameXmlEvent("TIME_PLAYED_MSG",
                              {std::to_string(data.totalTimePlayed),
                               std::to_string(data.levelTimePlayed)});
        // That event is the whole answer: the hand-built lines this client
        // wrote for its own chat window went with the window.
    }
}

// ============================================================
// Random Roll Handlers
// ============================================================

void SocialHandler::handleRandomRoll(network::Packet& packet) {
    RandomRollData data;
    if (!RandomRollParser::parse(packet, data)) return;
    std::string rollerName = (data.rollerGuid == queries_.characterGuid()) ? "You" : "Someone";
    if (data.rollerGuid != queries_.characterGuid()) {
        if (std::string name = queries_.playerName(data.rollerGuid); !name.empty())
            rollerName = std::move(name);
    }
    std::string msg = rollerName + ((data.rollerGuid == queries_.characterGuid()) ? " roll " : " rolls ");
    msg += std::to_string(data.result) + " (" + std::to_string(data.minRoll) + "-" + std::to_string(data.maxRoll) + ")";
    chat_.addSystemChatMessage(msg);
}

// ============================================================
// Logout Handlers
// ============================================================

void SocialHandler::handleLogoutResponse(network::Packet& packet) {
    LogoutResponseData data;
    if (!LogoutResponseParser::parse(packet, data)) return;
    if (data.result == 0) {
        if (data.instant) { chat_.addSystemChatMessage("Logging out..."); logoutCountdown_ = 0.0f; }
        else { chat_.addSystemChatMessage("Logging out in 20 seconds..."); logoutCountdown_ = 20.0f; }
        // An instant logout cannot be cancelled: the server is taking the
        // world away from this packet on.
        if (data.instant) commitLogout();
        if (frameXml_.frameXmlListening()) {
            frameXml_.fireFrameXmlEvent("PLAYER_LOGOUT", {});
            // The timer the server just imposed, announced so the interface can
            // show it. UIParent answers PLAYER_CAMPING with the CAMP popup and
            // PLAYER_QUITING with QUIT, both of which count down and offer to
            // cancel - and it already answers LOGOUT_CANCEL by hiding them, so
            // half of this conversation was wired and the half that starts it
            // was not. Nothing could show the twenty seconds but a line of chat.
            //
            // Which of the two is the difference between /logout and /quit,
            // which this handler already keeps in exitAfterLogout_.
            if (!data.instant) {
                frameXml_.fireFrameXmlEvent(
                    exitAfterLogout_ ? "PLAYER_QUITING" : "PLAYER_CAMPING", {});
            }
        }
    } else {
        // The server said no, and what that means depends on what was asked.
        //
        // For /logout it is the whole answer: the player stays in the world and
        // is told why. For Exit Game it is not. That button asks the program to
        // close, and a refusal governs whether the character leaves the world
        // tidily rather than whether this process keeps running - so printing
        // the error and staying put left a quit button that says no, which is
        // the one thing a quit button must never do.
        const bool exiting = exitAfterLogout_;
        loggingOut_ = false; exitAfterLogout_ = false; logoutCountdown_ = 0.0f;
        logoutCommitted_ = false;
        if (exiting) {
            LOG_WARNING("Logout refused, but Exit Game was asked for - closing anyway");
            events_.publish(events::ExitRequested{});
        } else {
            raiseUiError("Cannot logout right now.");
        }
    }
}

void SocialHandler::handleLogoutComplete(network::Packet& /*packet*/) {
    // The countdown finishing is not the end of it: the server says when the
    // character is actually out of the world, and only then can the client leave -
    // to character select, or out of the game entirely for /quit and /exit.
    const bool exiting = exitAfterLogout_;
    chat_.addSystemChatMessage("Logout complete.");
    loggingOut_ = false; exitAfterLogout_ = false; logoutCountdown_ = 0.0f;
    logoutCommitted_ = false;
    LOG_WARNING("Logout complete (exiting=", exiting ? "yes" : "no", ")");  // temporary: visible at default log level
    events_.publish(events::LogoutCompleted{exiting});
}

void SocialHandler::commitLogout() {
    if (logoutCommitted_) return;
    logoutCommitted_ = true;
    LOG_INFO("Logout committed (exiting=", exitAfterLogout_ ? "yes" : "no", ")");
    events_.publish(events::LogoutCommitted{exitAfterLogout_});
}

// ============================================================
// Instance Handlers
// ============================================================

void SocialHandler::handleRaidInstanceInfo(network::Packet& packet) {
    const bool isTbc = isActiveExpansion("tbc");
    const bool isClassic = isClassicLikeExpansion();
    const bool useTbcFormat = isTbc || isClassic;
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    instanceLockouts_.clear();
    instanceLockouts_.reserve(count);
    const size_t kEntrySize = useTbcFormat ? 13 : 18;
    for (uint32_t i = 0; i < count; ++i) {
        if (packet.getRemainingSize() < kEntrySize) break;
        InstanceLockout lo;
        lo.mapId = packet.readUInt32(); lo.difficulty = packet.readUInt32();
        if (useTbcFormat) { lo.resetTime = packet.readUInt32(); lo.locked = packet.readUInt8() != 0; lo.extended = false; }
        else { lo.resetTime = packet.readUInt64(); lo.locked = packet.readUInt8() != 0; lo.extended = packet.readUInt8() != 0; }
        instanceLockouts_.push_back(lo);
    }
}

void SocialHandler::handleInstanceDifficulty(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 4) return;
    uint32_t prevDifficulty = instanceDifficulty_;
    instanceDifficulty_ = packet.readUInt32();
    if (rem() >= 4) {
        uint32_t secondField = packet.readUInt32();
        if (rem() >= 4) instanceIsHeroic_ = (instanceDifficulty_ == 1);
        else instanceIsHeroic_ = (secondField != 0);
    } else {
        instanceIsHeroic_ = (instanceDifficulty_ == 1);
    }
    if (instanceDifficulty_ != prevDifficulty) {
        const char* diffLabel = instanceDifficultyName(instanceDifficulty_);
        if (diffLabel) chat_.addSystemChatMessage(std::string("Dungeon difficulty set to ") + diffLabel + ".");
        // Already noticed and already said - in chat, to the player. The
        // minimap hangs a difficulty banner off its own corner and rereads
        // GetInstanceInfo on this, which answers from the value just stored,
        // so the banner was the one thing not told the difficulty had changed.
        if (frameXml_.frameXmlListening()) {
            frameXml_.fireFrameXmlEvent("PLAYER_DIFFICULTY_CHANGED", {});
            frameXml_.fireFrameXmlEvent("UPDATE_INSTANCE_INFO", {});
        }
    }
}

// ============================================================
// LFG Handlers
// ============================================================
// These decode; the dungeon finder's state and what is said about it are the
// lfg context's.

void SocialHandler::handleLfgJoinResult(network::Packet& packet) {
    // Two uint32s, not two bytes: read as bytes, the state came from the
    // second byte of the result, which is zero.
    if (!packet.hasRemaining(8)) return;
    const uint32_t result = packet.readUInt32();
    const uint32_t state  = packet.readUInt32();
    auto* feed = lfgFeed(queries_);
    if (!feed) return;
    if (result == 0) feed->onJoined(state);
    else feed->onJoinRefused(lfgJoinRefusal(result));
}

// SMSG_LFG_PLAYER_INFO - what the random dungeons pay out, and what this
// character is not allowed to queue for.
//
// The reward block has to be walked even though the locks are what matter
// most, because they are behind it and its length depends on how many items
// each random dungeon rewards.
void SocialHandler::handleLfgPlayerInfo(network::Packet& packet) {
    if (!packet.hasRemaining(1)) { packet.skipAll(); return; }
    const uint8_t randomCount = packet.readUInt8();
    std::vector<lfg::LfgReward> rewards;
    for (uint8_t i = 0; i < randomCount; ++i) {
        if (!packet.hasRemaining(4 + 1 + 4 * 4 + 1)) { packet.skipAll(); return; }
        lfg::LfgReward r;
        r.dungeonId  = packet.readUInt32() & 0x00FFFFFFu;
        r.done       = packet.readUInt8() != 0;
        r.money      = packet.readUInt32();
        r.experience = packet.readUInt32();
        packet.readUInt32();          // the variable parts, which the server
        packet.readUInt32();          // sends as zero
        const uint8_t items = packet.readUInt8();
        for (uint8_t j = 0; j < items; ++j) {
            if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
            lfg::LfgRewardItem item;
            item.itemId        = packet.readUInt32();
            item.displayInfoId = packet.readUInt32();
            item.count         = packet.readUInt32();
            r.items.push_back(item);
        }
        rewards.push_back(std::move(r));
    }

    if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
    const uint32_t lockCount = packet.readUInt32();
    std::unordered_map<uint32_t, uint32_t> locks;
    for (uint32_t i = 0; i < lockCount && packet.hasRemaining(8); ++i) {
        // The entry packs the dungeon and its type; the list is keyed by the
        // dungeon alone, which is what the interface asks with.
        const uint32_t entry = packet.readUInt32();
        const uint32_t status = packet.readUInt32();
        locks[entry & 0x00FFFFFFu] = status;
    }
    packet.skipAll();

    LOG_INFO("SMSG_LFG_PLAYER_INFO: ", randomCount, " random dungeons, ", locks.size(), " locked");
    if (auto* feed = lfgFeed(queries_)) feed->onLockInfo(std::move(rewards), std::move(locks));
}

void SocialHandler::handleLfgQueueStatus(network::Packet& packet) {
    // Thirty-one bytes: dungeon, five wait times, three one-byte "needed"
    // counts, and the time queued. Every time is a difference of two time_t
    // values on the server - seconds, not milliseconds.
    if (!packet.hasRemaining(31)) return;
    lfg::QueueStatus status;
    status.dungeonId     = packet.readUInt32();
    status.avgWaitSec    = static_cast<int32_t>(packet.readUInt32());
    status.myWaitSec     = static_cast<int32_t>(packet.readUInt32());
    status.waitTank      = static_cast<int32_t>(packet.readUInt32());
    status.waitHealer    = static_cast<int32_t>(packet.readUInt32());
    status.waitDps       = static_cast<int32_t>(packet.readUInt32());
    status.needTank      = packet.readUInt8();
    status.needHealer    = packet.readUInt8();
    status.needDps       = packet.readUInt8();
    status.queuedSeconds = packet.readUInt32();
    if (auto* feed = lfgFeed(queries_)) feed->onQueueStatus(status);
}

void SocialHandler::handleLfgProposalUpdate(network::Packet& packet) {
    // dungeon(4), state(1), id(4), encounters(4), silent(1), size(1), then the
    // members. The state is ONE byte; read as a uint32 it was assembled from
    // the wrong bytes and every branch on it was taken at random.
    if (!packet.hasRemaining(15)) return;
    lfg::Proposal proposal;
    // The entry packs the dungeon and its type, as LFGDungeonData::Entry does.
    proposal.dungeonId  = packet.readUInt32() & 0x00FFFFFFu;
    proposal.state      = packet.readUInt8();
    proposal.proposalId = packet.readUInt32();
    /*uint32_t encountersDone =*/ packet.readUInt32();
    proposal.silent     = packet.readUInt8() != 0;
    const uint8_t groupSize = packet.hasRemaining(1) ? packet.readUInt8() : 0;
    for (uint8_t i = 0; i < groupSize && packet.hasRemaining(9); ++i) {
        lfg::LfgProposalMember m;
        m.role      = packet.readUInt32();
        m.isSelf    = packet.readUInt8() != 0;
        m.inDungeon = packet.readUInt8() != 0;
        m.sameGroup = packet.readUInt8() != 0;
        m.answered  = packet.readUInt8() != 0;
        m.accepted  = packet.readUInt8() != 0;
        proposal.members.push_back(m);
    }
    if (auto* feed = lfgFeed(queries_)) feed->onProposal(std::move(proposal));
}

void SocialHandler::handleLfgRoleCheckUpdate(network::Packet& packet) {
    // state is the uint32; the uint8 after it says whether the check is only
    // starting. Then the dungeons being checked for - each an LFGDungeons
    // entry with the type in its top byte - and how many are in the group.
    // GetLFGRoleUpdate needs the count: the popup's tooltip compares it.
    if (!packet.hasRemaining(6)) return;
    lfg::RoleCheck check;
    check.state = packet.readUInt32();
    packet.readUInt8();   // initiating
    const uint8_t dungeonCount = packet.readUInt8();
    for (uint8_t i = 0; i < dungeonCount && packet.hasRemaining(4); ++i) {
        const uint32_t entry = packet.readUInt32();
        check.dungeons.push_back({.dungeonId = entry & 0x00FFFFFFu,
                                  .typeId = static_cast<uint8_t>((entry >> 24) & 0xFFu)});
    }
    check.members = packet.hasRemaining(1) ? packet.readUInt8() : 0;
    if (auto* feed = lfgFeed(queries_)) feed->onRoleCheck(std::move(check));
}

/// SMSG_LFG_UPDATE_PLAYER and SMSG_LFG_UPDATE_PARTY.
///
/// Both open with the update type and a flag saying whether the rest of the
/// packet is there; the body is where the server says whether you are queued.
/// The flags decide the state, not the update type: under 3.3.5's
/// LfgUpdateType 15 is UPDATE_STATUS, the server restating the queue, and
/// reading it as "left the queue" threw queued players out locally.
void SocialHandler::handleLfgUpdatePlayer(network::Packet& packet) {
    //   uint8  updateType
    //   uint8  extraInfo
    //   if extraInfo:
    //     uint8  queued
    //     uint8  0
    //     uint8  0
    //     uint8  dungeonCount
    //     uint32 dungeon x dungeonCount
    //     cstring comment
    if (!packet.hasRemaining(2)) return;
    lfg::QueueUpdate update;
    update.updateType = packet.readUInt8();
    update.extraInfo = packet.readUInt8() != 0;
    if (update.extraInfo && packet.hasRemaining(4)) {
        update.queued = packet.readUInt8() != 0;
        packet.readUInt8();
        packet.readUInt8();
        update.dungeonId = readLfgFirstDungeon(packet);
    }
    if (auto* feed = lfgFeed(queries_)) feed->onQueueUpdate(update);
}

void SocialHandler::handleLfgUpdateParty(network::Packet& packet) {
    // The same opening, then a join flag ahead of the queued one and three
    // more bytes before the count.
    //
    //   uint8  updateType
    //   uint8  extraInfo
    //   if extraInfo:
    //     uint8  joined
    //     uint8  queued
    //     uint8  0
    //     uint8  0
    //     uint8  0 x3
    //     uint8  dungeonCount
    //     uint32 dungeon x dungeonCount
    //     cstring comment
    if (!packet.hasRemaining(2)) return;
    lfg::QueueUpdate update;
    update.updateType = packet.readUInt8();
    update.extraInfo = packet.readUInt8() != 0;
    bool joined = false;
    if (update.extraInfo && packet.hasRemaining(8)) {
        joined = packet.readUInt8() != 0;
        update.queued = packet.readUInt8() != 0;
        for (int i = 0; i < 5; ++i) packet.readUInt8();
        update.dungeonId = readLfgFirstDungeon(packet);
    }
    // The party's join flag covers being in the dungeon as well as being in the
    // queue, so it only means "in a dungeon" when the queue flag is off.
    update.inDungeon = joined && !update.queued;
    if (auto* feed = lfgFeed(queries_)) feed->onQueueUpdate(update);
}

/// The dungeons an update is about. The first is the one the queue window
/// names; the comment after them is the group's listing text, which this
/// client does not offer a way to set.
std::optional<uint32_t> SocialHandler::readLfgFirstDungeon(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return std::nullopt;
    const uint8_t count = packet.readUInt8();
    std::optional<uint32_t> first;
    for (uint8_t i = 0; i < count && packet.hasRemaining(4); ++i) {
        const uint32_t dungeonEntry = packet.readUInt32();
        if (i == 0) first = dungeonEntry & 0x00FFFFFFu;
    }
    return first;
}

void SocialHandler::handleLfgPlayerReward(network::Packet& packet) {
    // SMSG_LFG_PLAYER_REWARD, against WorldSession::SendLfgPlayerReward:
    //
    //   uint32 rdungeonEntry   the random entry queued for
    //   uint32 sdungeonEntry   the dungeon actually finished
    //   uint8  done
    //   uint32 1               a constant the server writes and nothing reads
    //   uint32 money           GetRewOrReqMoney(level), in copper
    //   uint32 xp              XPValue(level)
    //   uint32 0
    //   uint32 0
    //   uint8  itemNum
    //   itemNum x { uint32 itemId, uint32 displayId, uint32 count }
    //
    // packet_layout_check covers this opcode and could not have caught the
    // constant being read as the money: it compares field widths, and every
    // field that slid was four bytes wide.
    if (!packet.hasRemaining(4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1)) return;
    lfg::LfgCompletionReward reward;
    reward.randomDungeonId = packet.readUInt32();
    reward.dungeonId       = packet.readUInt32();
    reward.done            = packet.readUInt8() != 0;
    packet.readUInt32();   // the constant 1
    reward.money           = packet.readUInt32();
    reward.xp              = packet.readUInt32();
    packet.readUInt32();
    packet.readUInt32();
    const uint8_t itemNum = packet.readUInt8();
    for (uint8_t i = 0; i < itemNum && packet.hasRemaining(12); ++i) {
        lfg::LfgCompletionReward::Item item;
        item.itemId    = packet.readUInt32();
        item.displayId = packet.readUInt32();
        item.count     = packet.readUInt32();
        reward.items.push_back(item);
    }
    if (auto* feed = lfgFeed(queries_)) feed->onReward(std::move(reward));
}

void SocialHandler::handleLfgBootProposalUpdate(network::Packet& packet) {
    // Three flags, the victim's guid, four counts, then the reason - the only
    // string; AzerothCore writes `data << boot.victim` and the reason, and
    // nothing else.
    if (!packet.hasRemaining(27)) return;
    lfg::BootVote vote;
    vote.inProgress  = packet.readUInt8() != 0;
    vote.didVote     = packet.readUInt8() != 0;
    vote.myVote      = packet.readUInt8() != 0;
    vote.victimGuid  = packet.readUInt64();
    vote.totalVotes  = packet.readUInt32();
    vote.bootVotes   = packet.readUInt32();
    vote.timeLeft    = packet.readUInt32();
    vote.votesNeeded = packet.readUInt32();
    if (packet.getReadPos() < packet.getSize()) vote.reason = packet.readString();
    if (auto* feed = lfgFeed(queries_)) feed->onBootVote(std::move(vote));
}

void SocialHandler::handleLfgTeleportDenied(network::Packet& packet) {
    // A uint32; every reason is a small number, which is how a one-byte read
    // used to get away with it.
    if (!packet.hasRemaining(4)) return;
    const uint32_t reason = packet.readUInt32();
    if (auto* feed = lfgFeed(queries_)) feed->onTeleportDenied(lfgTeleportRefusal(reason));
}

void SocialHandler::updateLogoutCountdown(float deltaTime) {
    // A join the server lets pass without a word is said after a while.
    if (auto* feed = battlegroundFeed(queries_)) feed->timePassed(deltaTime);
    if (loggingOut_ && logoutCountdown_ > 0.0f) {
        logoutCountdown_ -= deltaTime;
        if (logoutCountdown_ <= 0.0f) {
            logoutCountdown_ = 0.0f;
            // The countdown ran out: past the point a cancel could reach the
            // server in time.
            commitLogout();
        }
    }
    // The bind prompt expires on its own; the server stops asking and the
    // pending bind lapses, so the dialog has to stop offering an answer.
    if (instanceLock_.active) {
        instanceLock_.secondsLeft -= deltaTime;
        if (instanceLock_.secondsLeft <= 0.0f) {
            instanceLock_.secondsLeft = 0.0f;
            instanceLock_.active = false;
            frameXml_.fireFrameXmlEvent("INSTANCE_LOCK_STOP", {});
        }
    }
}

void SocialHandler::respondInstanceLock(bool accept) {
    // Accepting saves the player to the instance; declining sends them to the
    // graveyard. The server acts on whichever arrives and clears the pending
    // bind, so sending twice is worse than not sending at all.
    if (!instanceLock_.active || !sender_.connected()) return;
    instanceLock_.active = false;

    network::Packet resp(wireOpcode(Opcode::CMSG_INSTANCE_LOCK_RESPONSE));
    resp.writeUInt8(accept ? 1 : 0);
    sender_.send(resp);

    LOG_INFO("CMSG_INSTANCE_LOCK_RESPONSE: ", accept ? "accepted" : "declined");
    frameXml_.fireFrameXmlEvent("INSTANCE_LOCK_STOP", {});
}

void SocialHandler::resetTransferState() {
    instanceLock_ = InstanceLockPrompt{};
    encounterUnitGuids_.fill(0);
    if (auto* feed = groupFeed(queries_)) feed->onWorldTransfer();
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

// ============================================================
// Minimap Ping
// ============================================================

void SocialHandler::sendMinimapPing(float wowX, float wowY) {
    if (!sender_.characterInWorld()) return;

    // MSG_MINIMAP_PING (CMSG direction): float posX + float posY
    // Server convention: posX = east/west axis = canonical Y (west)
    //                    posY = north/south axis = canonical X (north)
    const float serverX = wowY;  // canonical Y (west) → server posX
    const float serverY = wowX;  // canonical X (north) → server posY

    network::Packet pkt(wireOpcode(Opcode::MSG_MINIMAP_PING));
    pkt.writeFloat(serverX);
    pkt.writeFloat(serverY);
    sender_.send(pkt);

    // Add ping locally so the sender sees their own ping immediately
    MinimapPing localPing;
    localPing.senderGuid = queries_.characterGuid();
    localPing.wowX       = wowX;
    localPing.wowY       = wowY;
    localPing.age        = 0.0f;
    minimapPings_.push_back(localPing);
}

// ============================================================
// Summon Request
// ============================================================

void SocialHandler::handleSummonRequest(network::Packet& packet) {
    if (!packet.hasRemaining(16)) return;

    summonerGuid_        = packet.readUInt64();
    uint32_t zoneId             = packet.readUInt32();
    uint32_t timeoutMs          = packet.readUInt32();
    summonTimeoutSec_    = timeoutMs / 1000.0f;
    pendingSummonRequest_= true;

    // The summoner in view, or the name the cache has for them.
    summonerName_ = queries_.playerName(summonerGuid_);
    if (summonerName_.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX",
                      static_cast<unsigned long long>(summonerGuid_));
        summonerName_ = tmp;
    }

    std::string msg = summonerName_ + " is summoning you";
    std::string zoneName = queries_.areaName(zoneId);
    if (!zoneName.empty())
        msg += " to " + zoneName;
    msg += '.';
    chat_.addSystemChatMessage(msg);
    LOG_INFO("SMSG_SUMMON_REQUEST: summoner=", summonerName_,
             " zoneId=", zoneId, " timeout=", summonTimeoutSec_, "s");
    frameXml_.fireFrameXmlEvent("CONFIRM_SUMMON", {});
}

/// Both answers to a summon: who summoned, then yes or no.
///
/// HandleSummonResponseOpcode reads `ObjectGuid summoner_guid` and then a bool,
/// nine bytes. Both answers were a single byte, so the server had one byte
/// where it wanted eight, ran off the end of the buffer and dropped the packet
/// - accepting a summon has never once reached it, and neither has declining
/// one. The same mistake as the battlefield answers below, made next door.
///
/// The guid is the one SMSG_SUMMON_REQUEST arrived with, which is already kept
/// so the message can name whoever cast it.
void SocialHandler::sendSummonResponse(bool accept) {
    if (!sender_.connected()) return;
    pendingSummonRequest_ = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SUMMON_RESPONSE));
    pkt.writeUInt64(summonerGuid_);
    pkt.writeUInt8(accept ? 1 : 0);
    sender_.send(pkt);
}

void SocialHandler::acceptSummon() {
    if (!pendingSummonRequest_) return;
    sendSummonResponse(true);
    chat_.addSystemChatMessage("Accepting summon...");
    LOG_INFO("Accepted summon from ", summonerName_);
}

void SocialHandler::declineSummon() {
    sendSummonResponse(false);
    chat_.addSystemChatMessage("Summon declined.");
}

// ============================================================
// Battlefield Manager
// ============================================================

/// All three battlefield answers carry the battle id first.
///
/// HandleBfEntryInviteResponse reads `uint32 BattleId >> uint8 Accepted` and
/// the exit request reads a battle id alone. Both answers used to be a single
/// byte, so the server read the accept flag as the low byte of a battle id,
/// ran out of buffer and dropped the packet - accepting a Wintergrasp
/// invitation had never once reached it.
void SocialHandler::sendBfMgrResponse(Opcode op, uint32_t battleId, bool accept,
                                      bool withFlag) {
    if (!sender_.characterInWorld()) return;
    network::Packet pkt(wireOpcode(op));
    pkt.writeUInt32(battleId);
    if (withFlag) pkt.writeUInt8(accept ? 1 : 0);
    sender_.send(pkt);
}

void SocialHandler::acceptBfMgrInvite() {
    if (!bfMgrInvitePending_) return;
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE,
                      bfMgrBattleId_, true, true);
    bfMgrInvitePending_ = false;
    LOG_INFO("acceptBfMgrInvite: battle ", bfMgrBattleId_);
}

void SocialHandler::declineBfMgrInvite() {
    if (!bfMgrInvitePending_) return;
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE,
                      bfMgrBattleId_, false, true);
    bfMgrInvitePending_ = false;
    LOG_INFO("declineBfMgrInvite: battle ", bfMgrBattleId_);
}

void SocialHandler::respondBfMgrQueueInvite(uint32_t battleId, bool accept) {
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_QUEUE_INVITE_RESPONSE,
                      battleId, accept, true);
    bfMgrInvitePending_ = false;
    LOG_INFO("respondBfMgrQueueInvite: battle ", battleId,
             accept ? " accepted" : " declined");
}

void SocialHandler::requestBfMgrExit(uint32_t battleId) {
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_EXIT_REQUEST,
                      battleId, false, false);
    LOG_INFO("requestBfMgrExit: battle ", battleId);
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void SocialHandler::sendSetDifficulty(uint32_t difficulty, bool raid) {
    if (!sender_.characterInWorld()) {
        LOG_WARNING("Cannot change difficulty: not in world");
        return;
    }

    // MSG_SET_DUNGEON_DIFFICULTY, not CMSG_CHANGEPLAYER_DIFFICULTY. The latter
    // is a real opcode number and the server registers it with Handle_NULL and
    // STATUS_NEVER - it is read off the wire and discarded - so every
    // difficulty change ever sent from here went nowhere. Both of the real ones
    // carry a single uint32 mode.
    const Opcode op = raid ? Opcode::MSG_SET_RAID_DIFFICULTY
                           : Opcode::MSG_SET_DUNGEON_DIFFICULTY;
    network::Packet packet(wireOpcode(op));
    packet.writeUInt32(difficulty);
    sender_.send(packet);
    LOG_INFO(raid ? "MSG_SET_RAID_DIFFICULTY sent: difficulty="
                  : "MSG_SET_DUNGEON_DIFFICULTY sent: difficulty=", difficulty);
}

void SocialHandler::toggleHelm() {
    if (!sender_.characterInWorld()) {
        LOG_WARNING("Cannot toggle helm: not in world or not connected");
        return;
    }

    const bool shown = !queries_.showingHelm();
    queries_.setShowingHelm(shown);
    sender_.send(ShowingHelmPacket::build(shown));
    chat_.addSystemChatMessage(shown ? "Helm is now visible." : "Helm is now hidden.");
    // The flag alone changed nothing on screen: it was set, sent to the server
    // and never read. Mark equipment dirty so the helm model and the hair under
    // it are both rebuilt from it.
    queries_.markOnlineEquipmentDirty();
    LOG_INFO("Helm visibility toggled: ", shown);
}

void SocialHandler::toggleCloak() {
    if (!sender_.characterInWorld()) {
        LOG_WARNING("Cannot toggle cloak: not in world or not connected");
        return;
    }

    const bool shown = !queries_.showingCloak();
    queries_.setShowingCloak(shown);
    sender_.send(ShowingCloakPacket::build(shown));
    chat_.addSystemChatMessage(shown ? "Cloak is now visible." : "Cloak is now hidden.");
    LOG_INFO("Cloak visibility toggled: ", shown);
}

void SocialHandler::setStandState(uint8_t standState) {
    if (!sender_.characterInWorld()) {
        LOG_WARNING("Cannot change stand state: not in world or not connected");
        return;
    }

    auto packet = StandStateChangePacket::build(standState);
    sender_.send(packet);
    LOG_INFO("Changed stand state to: ", static_cast<int>(standState));
}

void SocialHandler::sendAlterAppearance(uint32_t hairStyleEntry, uint32_t hairColor,
                                        uint32_t facialHairEntry, uint32_t skinColorEntry) {
    if (!sender_.characterInWorld()) return;
    auto pkt = AlterAppearancePacket::build(hairStyleEntry, hairColor,
                                            facialHairEntry, skinColorEntry);
    sender_.send(pkt);
    LOG_INFO("sendAlterAppearance: hairEntry=", hairStyleEntry,
             " color=", hairColor, " facialEntry=", facialHairEntry,
             " skinEntry=", skinColorEntry);
}

void SocialHandler::deleteGmTicket() {
    if (!sender_.characterInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_DELETETICKET));
    sender_.send(pkt);
    // The ticket goes when the server says it has (SMSG_GMTICKET_DELETETICKET).
    LOG_INFO("Deleting GM ticket");
}

// MSG_GUILD_EVENT_LOG_QUERY: ask, with no payload. The reply is the same
// opcode and is parsed below.
void SocialHandler::requestGuildBankLog(uint8_t tab) {
    if (!sender_.characterInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_GUILD_BANK_LOG_QUERY));
    pkt.writeUInt8(tab);
    sender_.send(pkt);
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY: tab ", static_cast<int>(tab));
}

// The reply, which shares its opcode with the request.
//
//   uint8 tab
//   uint8 count
//   per entry:
//     int8   type
//     uint64 playerGuid
//     the payload the type calls for - an item and a count, those plus a
//     destination tab for a move, or a sum of money for everything else
//     uint32 secondsAgo
//
// Nothing sent the query, so nothing ever arrived to parse: the guild bank's
// log tab counted zero transactions and drew an empty page over a log the
// server keeps.
void SocialHandler::handleGuildBankLog(network::Packet& packet) {
    if (!packet.hasRemaining(2)) { packet.skipAll(); return; }
    const uint8_t tab = packet.readUInt8();
    const uint8_t count = packet.readUInt8();
    if (tab >= guildBankLogs_.size()) { packet.skipAll(); return; }

    auto& log = guildBankLogs_[tab];
    log.clear();
    log.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(9)) break;
        GuildBankLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        switch (e.type) {
            case 1: case 2:                 // deposit / withdraw an item
                if (!packet.hasRemaining(8)) { packet.skipAll(); return; }
                e.itemId = packet.readUInt32();
                e.count  = packet.readUInt32();
                break;
            case 3: case 7:                 // moved between tabs
                if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
                e.itemId   = packet.readUInt32();
                e.count    = packet.readUInt32();
                e.otherTab = packet.readUInt8();
                break;
            default:                        // money, in copper
                if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
                e.count = packet.readUInt32();
                break;
        }
        if (!packet.hasRemaining(4)) break;
        e.secondsAgo = packet.readUInt32();
        if (e.itemId) queries_.ensureItemInfo(e.itemId);
        log.push_back(e);
    }
    packet.skipAll();
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY reply: tab ", static_cast<int>(tab),
             ", ", log.size(), " entries");
    if (frameXml_.frameXmlListening()) {
        frameXml_.fireFrameXmlEvent("GUILDBANKLOG_UPDATE", {});
    }
}

void SocialHandler::requestGuildEventLog() {
    if (!sender_.characterInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_GUILD_EVENT_LOG_QUERY));
    sender_.send(pkt);
}

// The reply. Two of the five fields are conditional, which is the whole
// difficulty: a reader that always takes the second guid loses eight bytes on
// every join and leave, and those are most of a guild log.
//
// Types, from AzerothCore's GuildEventLogTypes: 1 invite, 2 join, 3 promote,
// 4 demote, 5 remove, 6 leave.
void SocialHandler::handleGuildEventLog(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 1) return;
    const uint8_t count = packet.readUInt8();
    guildEventLog_.clear();
    guildEventLog_.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (rem() < 9) break;                 // type + one guid at minimum
        GuildEventLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        // Everything but join (2) and leave (6) names a second player.
        if (e.type != 2 && e.type != 6) {
            if (rem() < 8) break;
            e.otherGuid = packet.readUInt64();
        }
        // Promote (3) and demote (4) carry the new rank.
        if (e.type == 3 || e.type == 4) {
            if (rem() < 1) break;
            e.newRank = packet.readUInt8();
        }
        if (rem() < 4) break;
        e.secondsAgo = packet.readUInt32();
        guildEventLog_.push_back(e);
    }
    LOG_INFO("Guild event log: ", guildEventLog_.size(), " entries");
    if (frameXml_.frameXmlListening()) frameXml_.fireFrameXmlEvent("GUILD_EVENT_LOG_UPDATE", {});
}

// CMSG_GMTICKET_SYSTEMSTATUS: is the ticket queue open at all. The reply,
// SMSG_GMTICKET_SYSTEMSTATUS, was already parsed - asking was the missing half,
// and the help frame asks every time it is shown.
void SocialHandler::requestGmSystemStatus() {
    if (!sender_.characterInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_SYSTEMSTATUS));
    sender_.send(pkt);
}

void SocialHandler::requestGmTicket() {
    if (!sender_.characterInWorld()) return;
    // CMSG_GMTICKET_GETTICKET has no payload - server responds with SMSG_GMTICKET_GETTICKET
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_GETTICKET));
    sender_.send(pkt);
    LOG_DEBUG("Sent CMSG_GMTICKET_GETTICKET - querying open ticket status");
}

// ============================================================
// Summons, minimap pings and the outdoor battlefield (moved from GameHandler)
// ============================================================

void SocialHandler::registerBattlefieldOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_SUMMON_REQUEST] = [this](network::Packet& p) { handleSummonRequest(p); };
    table[Opcode::SMSG_SUMMON_CANCEL] = [this](network::Packet& /*packet*/) {
        pendingSummonRequest_ = false;
        // handleSummonRequest raised the prompt with CONFIRM_SUMMON; uiparent.lua
        // takes this one down again. Told only in chat, the dialog stays on
        // screen offering a summon the server has already withdrawn.
        frameXml_.fireFrameXmlEvent("CANCEL_SUMMON", {});
        chat_.addSystemChatMessage("Summon cancelled.");
    };

    table[Opcode::MSG_MINIMAP_PING] = [this](network::Packet& packet) {
        const bool mmTbcLike = isPreWotlk();
        if (!packet.hasRemaining(mmTbcLike ? 8u : 1u) ) return;
        uint64_t senderGuid = mmTbcLike
            ? packet.readUInt64() : packet.readPackedGuid();
        if (!packet.hasRemaining(8)) return;
        float pingX = packet.readFloat();
        float pingY = packet.readFloat();
        MinimapPing ping;
        ping.senderGuid = senderGuid;
        ping.wowX = pingY;
        ping.wowY = pingX;
        ping.age  = 0.0f;
        minimapPings_.push_back(ping);
        if (senderGuid != queries_.characterGuid()) {
            events_.publish(events::MinimapPinged{senderGuid, ping.wowX, ping.wowY});
        }
        // The ping is drawn by this client from minimapPings_ and was never
        // announced, so a handed-over minimap heard the sound and showed
        // nothing. The interface wants the unit that pinged and the position,
        // and marks the spot itself.
        //
        // x then y, in that order, which is the reverse of how they are stored
        // above: the wire sends them the other way round and this client swaps
        // them on the way in. Passing the stored pair straight through would
        // put every ping at its own mirror image.
        std::string unitId = queries_.unitIdFor(senderGuid);
        if (unitId.empty()) unitId = "player";
        frameXml_.fireFrameXmlEvent("MINIMAP_PING", {unitId,
                                                     std::to_string(ping.wowY),
                                                     std::to_string(ping.wowX)});
    };

    // uint64 battlefieldGuid + uint32 zoneId + uint64 expireUnixTime (seconds)
    table[Opcode::SMSG_BATTLEFIELD_MGR_ENTRY_INVITE] = [this](network::Packet& packet) {
        // uint32 battleId + uint32 zoneId + uint32 acceptTime - twelve bytes.
        //
        // This required twenty and read a 64-bit guid and a 64-bit time that
        // the server does not send, so it returned on the length check every
        // time and the handler never ran once. The event was written down as
        // never fired, which was true and was the symptom rather than the
        // cause.
        if (!packet.hasRemaining(12)) {
            packet.skipAll(); return;
        }
        const uint32_t bfBattleId = packet.readUInt32();
        const uint32_t bfZoneId   = packet.readUInt32();
        const uint32_t expireTime = packet.readUInt32();
        (void)expireTime;
        bfMgrBattleId_ = bfBattleId;
        // Store the invitation so the UI can show a prompt
        bfMgrInvitePending_ = true;
        bfMgrZoneId_        = bfZoneId;
        char buf[128];
        std::string bfZoneName = queries_.areaName(bfZoneId);
        if (!bfZoneName.empty())
            std::snprintf(buf, sizeof(buf),
                "You are invited to the outdoor battlefield in %s. Click to enter.",
                bfZoneName.c_str());
        else
            std::snprintf(buf, sizeof(buf),
                "You are invited to the outdoor battlefield in zone %u. Click to enter.",
                bfZoneId);
        chat_.addSystemChatMessage(buf);
        // The prompt to enter is a static popup the interface raises from this.
        // The zone id goes with it as the battle id: every handler in the group
        // opens `local battleID = ...` and passes it back when the player
        // answers, so a bare fire would answer for no battlefield.
        if (frameXml_.frameXmlListening())
            frameXml_.fireFrameXmlEvent("BATTLEFIELD_MGR_ENTRY_INVITE", {std::to_string(bfBattleId)});
        LOG_INFO("SMSG_BATTLEFIELD_MGR_ENTRY_INVITE: zoneId=", bfZoneId);
    };
    table[Opcode::SMSG_BATTLEFIELD_MGR_ENTERED] = [this](network::Packet& packet) {
        // uint32 battleId, then three flags - seven bytes. This read a 64-bit
        // guid the server does not send and guarded eight, so on a seven-byte
        // packet it never ran: entering Wintergrasp announced nothing.
        if (packet.hasRemaining(7)) {
            const uint32_t bfBattleId = packet.readUInt32();
            bfMgrBattleId_ = bfBattleId;
            packet.readUInt8();   // unk, always one
            uint8_t isSafe  = packet.readUInt8();
            uint8_t onQueue = packet.readUInt8();
            bfMgrInvitePending_ = false;
            chat_.addSystemChatMessage(isSafe ? "You are in the battlefield zone (safe area)."
                                              : "You have entered the battlefield!");
            if (onQueue) chat_.addSystemChatMessage("You are in the battlefield queue.");
            if (frameXml_.frameXmlListening())
                frameXml_.fireFrameXmlEvent("BATTLEFIELD_MGR_ENTERED", {std::to_string(bfMgrZoneId_)});
            LOG_INFO("SMSG_BATTLEFIELD_MGR_ENTERED: isSafe=", static_cast<int>(isSafe), " onQueue=", static_cast<int>(onQueue));
        }
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint32 battlefieldId + uint64 expireTime
    table[Opcode::SMSG_BATTLEFIELD_MGR_QUEUE_INVITE] = [this](network::Packet& packet) {
        // uint32 battleId + uint8 warmup - five bytes, and this required
        // twenty. Same shape as the entry invite above: never ran, and the
        // event it fires was recorded as one the server never sends.
        if (!packet.hasRemaining(5)) {
            packet.skipAll(); return;
        }
        const uint32_t bfId = packet.readUInt32();
        const bool warmup = packet.readUInt8() != 0;
        bfMgrBattleId_ = bfId;
        bfMgrInvitePending_ = true;
        bfMgrZoneId_        = bfId;
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "A spot has opened in the battlefield queue (battlefield %u).", bfId);
        chat_.addSystemChatMessage(buf);
        if (frameXml_.frameXmlListening())
            // The warmup flag picks between two dialogs and was read off the
            // wire and thrown away, so the warmup one could never appear.
            frameXml_.fireFrameXmlEvent("BATTLEFIELD_MGR_QUEUE_INVITE",
                                        {std::to_string(bfId), eventBool(warmup)});
        LOG_INFO("SMSG_BATTLEFIELD_MGR_QUEUE_INVITE: bfId=", bfId);
    };
    // uint32 battlefieldId + uint32 teamId + uint8 accepted + uint8 loggingEnabled + uint8 result
    // result: 0=queued, 1=not_in_group, 2=too_high_level, 3=too_low_level,
    //         4=in_cooldown, 5=queued_other_bf, 6=bf_full
    table[Opcode::SMSG_BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(11)) {
            packet.skipAll(); return;
        }
        uint32_t bfId2    = packet.readUInt32();
        /*uint32_t teamId =*/ packet.readUInt32();
        uint8_t accepted  = packet.readUInt8();
        /*uint8_t logging =*/ packet.readUInt8();
        uint8_t result    = packet.readUInt8();
        if (accepted) {
            chat_.addSystemChatMessage("You have joined the battlefield queue.");
        } else {
            static const char* kBfQueueErrors[] = {
                "Queued for battlefield.", "Not in a group.", "Level too high.",
                "Level too low.", "Battlefield in cooldown.", "Already queued for another battlefield.",
                "Battlefield is full."
            };
            const char* msg = (result < 7) ? kBfQueueErrors[result]
                                           : "Battlefield queue request failed.";
            chat_.addSystemChatMessage(std::string("Battlefield: ") + msg);
        }
        // The battle id and whether the queue took, in that order - the
        // interface reads both to say which battlefield answered and how.
        if (frameXml_.frameXmlListening()) {
            frameXml_.fireFrameXmlEvent("BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE",
                                        {std::to_string(bfId2), std::to_string(accepted)});
        }
        LOG_INFO("SMSG_BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE: accepted=", static_cast<int>(accepted),
                 " result=", static_cast<int>(result));
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint8 remove
    table[Opcode::SMSG_BATTLEFIELD_MGR_EJECT_PENDING] = [this](network::Packet& packet) {
        // A battle id and nothing else. This read a packed pair of a guid and a
        // flag across nine bytes, so the guard alone kept it from running.
        //
        // AzerothCore declares this opcode and never sends it, so nothing here
        // has been exercised - but a wrong reading is worse than an untested
        // one, and the frame that answers it is live.
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        const uint32_t battleId = packet.readUInt32();
        packet.skipAll();

        chat_.addSystemChatMessage("You will be removed from the battlefield shortly.");
        LOG_INFO("SMSG_BATTLEFIELD_MGR_EJECT_PENDING: battle ", battleId);
        // The second argument picks between two dialogs and nothing here knows
        // which; nil is the local one, which is what a player being ejected
        // from the battlefield they are standing in should see.
        frameXml_.fireFrameXmlEvent("BATTLEFIELD_MGR_EJECT_PENDING",
                                    {std::to_string(battleId), kEventNil});
    };
    // uint64 battlefieldGuid + uint32 reason + uint32 battleStatus + uint8 relocated
    table[Opcode::SMSG_BATTLEFIELD_MGR_EJECTED] = [this](network::Packet& packet) {
        // Seven bytes: a battle id, then three single bytes. SendBfLeaveMessage
        // writes exactly that.
        //
        // This guarded seventeen and read a guid, two thirty-two-bit fields and
        // a flag, so it never ran once - and the reason it treated as a small
        // enum is a bit mask, which would have named the wrong thing if it had.
        if (!packet.hasRemaining(7)) { packet.skipAll(); return; }
        const uint32_t battleId = packet.readUInt32();
        const uint8_t reason = packet.readUInt8();
        const uint8_t battleStatus = packet.readUInt8();
        const uint8_t relocated = packet.readUInt8();
        packet.skipAll();

        constexpr uint8_t kClose = 0x01, kExited = 0x08, kLowLevel = 0x10;
        const bool exited = (reason & kExited) != 0;
        const bool lowLevel = (reason & kLowLevel) != 0;

        if (lowLevel) {
            raiseUiError("You are not high enough level for this battlefield.");
        } else if (reason & kClose) {
            chat_.addSystemChatMessage("The battlefield has closed.");
        } else if (exited) {
            chat_.addSystemChatMessage("You have left the battlefield.");
        } else {
            chat_.addSystemChatMessage("You have been removed from the battlefield.");
        }

        LOG_INFO("SMSG_BATTLEFIELD_MGR_EJECTED: battle ", battleId,
                 " reason 0x", std::hex, static_cast<int>(reason), std::dec,
                 " status ", static_cast<int>(battleStatus),
                 relocated ? " (relocated)" : "");

        bfMgrInvitePending_ = false;

        // battleID, playerExited, relocated, battleActive, lowLevel. The four
        // flags are booleans in the middle of the list, so they go through
        // eventBool - a zero here would read as true and pick the wrong dialog.
        frameXml_.fireFrameXmlEvent("BATTLEFIELD_MGR_EJECTED",
                                    {std::to_string(battleId),
                                     eventBool(exited),
                                     eventBool(relocated != 0),
                                     eventBool(battleStatus != 0),
                                     eventBool(lowLevel)});
    };
    // uint32 oldState + uint32 newState
    // States: 0=Waiting, 1=Starting, 2=InProgress, 3=Ending, 4=Cooldown
    table[Opcode::SMSG_BATTLEFIELD_MGR_STATE_CHANGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            /*uint32_t oldState =*/ packet.readUInt32();
            uint32_t newState   = packet.readUInt32();
            static const char* kBfStates[] = {
                "waiting", "starting", "in progress", "ending", "in cooldown"
            };
            const char* stateStr = (newState < 5) ? kBfStates[newState] : "unknown state";
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Battlefield is now %s.", stateStr);
            chat_.addSystemChatMessage(buf);
            LOG_INFO("SMSG_BATTLEFIELD_MGR_STATE_CHANGE: newState=", newState);
        }
        packet.skipAll();
    };
}

} // namespace game
} // namespace wowee
