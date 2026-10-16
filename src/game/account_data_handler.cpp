#include "game/account_data_handler.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_events.hpp"
#include "game/game_utils.hpp"
#include "game/packet_sender.hpp"
#include "game/world_packets.hpp"
#include "core/event_bus.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace wowee {
namespace game {

namespace {

/// The file each slot is kept in on disk - the original client's own names.
const char* cacheFileName(AccountDataType type) {
    switch (type) {
        case AccountDataType::GlobalConfig:
        case AccountDataType::CharacterConfig:   return "config-cache.wtf";
        case AccountDataType::GlobalBindings:
        case AccountDataType::CharacterBindings: return "bindings-cache.wtf";
        case AccountDataType::GlobalMacros:
        case AccountDataType::CharacterMacros:   return "macros-cache.txt";
        case AccountDataType::CharacterLayout:   return "layout-cache.txt";
        case AccountDataType::CharacterChat:     return "chat-cache.txt";
    }
    return "unknown-cache.txt";
}

/// The first line of every cached slot; the text follows it unchanged.
constexpr std::string_view kCacheHeader = "WOWEE-ACCOUNT-DATA ";

AccountDataType typeOf(int index) { return static_cast<AccountDataType>(index); }

AccountDataType macroSlot(bool character) {
    return character ? AccountDataType::CharacterMacros : AccountDataType::GlobalMacros;
}

/// The macro frame lists each half by name, without regard to case; equal
/// names keep their ids' order so the list never reshuffles between reads.
bool macroOrder(const Macro& a, const Macro& b) {
    const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    const size_t n = std::min(a.name.size(), b.name.size());
    for (size_t i = 0; i < n; ++i) {
        const char ca = lower(static_cast<unsigned char>(a.name[i]));
        const char cb = lower(static_cast<unsigned char>(b.name[i]));
        if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
    }
    if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
    return a.id < b.id;
}

/// An icon as macros-cache.txt names it: the interface hands over a texture
/// path, "Interface\Icons\Spell_Nature_Lightning", and the file keeps only
/// the last part.
std::string iconName(const std::string& icon) {
    const size_t sep = icon.find_last_of("\\/");
    return sep == std::string::npos ? icon : icon.substr(sep + 1);
}

/// A path component from a name the server chose: kept readable, with
/// anything a file system could refuse replaced.
std::string pathSafe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out += (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '@')
                   ? static_cast<char>(c) : '_';
    }
    return out;
}

} // namespace

AccountDataHandler::AccountDataHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                                       core::EventBus& events, std::string cacheRoot)
    : sender_(sender), frameXml_(frameXml), events_(events), cacheRoot_(std::move(cacheRoot)) {}

void AccountDataHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_ACCOUNT_DATA_TIMES] = [this](network::Packet& p) { handleAccountDataTimes(p); };
    table[Opcode::SMSG_UPDATE_ACCOUNT_DATA] = [this](network::Packet& p) { handleUpdateAccountData(p); };
    table[Opcode::SMSG_UPDATE_ACCOUNT_DATA_COMPLETE] = [this](network::Packet& p) {
        handleUpdateAccountDataComplete(p);
    };
}

// ---- Session ----------------------------------------------------------------

void AccountDataHandler::beginSession(const std::string& accountName, const std::string& serverHost) {
    std::string account = accountName;
    for (char& c : account) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    accountKey_ = pathSafe(account + "@" + serverHost);
    characterGuid_ = 0;
    slots_ = {};
    macros_ = {};
    legacyMacros_ = {};
}

void AccountDataHandler::accountAuthenticated() {
    for (int i = 0; i < kAccountDataTypeCount; ++i) {
        if (isCharacterAccountData(typeOf(i))) continue;
        slots_[i] = {};
        loadFromCache(typeOf(i));
        loaded(typeOf(i));
    }
    // The server answers with when its own copies changed, and the slots
    // newer there are asked for then. TrinityCore and AzerothCore also send
    // the times unasked as the session opens; a slot is asked for once
    // however many times it is announced.
    if (serverKeepsAccountData()) {
        if (sender_.connected()) sender_.send(ReadyForAccountDataTimesPacket::build());
    }
}

void AccountDataHandler::characterSelected(uint64_t characterGuid) {
    characterGuid_ = characterGuid;
    legacyMacros_ = {};
    for (int i = 0; i < kAccountDataTypeCount; ++i) {
        if (!isCharacterAccountData(typeOf(i))) continue;
        slots_[i] = {};
        loadFromCache(typeOf(i));
        // Said even when empty: the previous character's bindings and macros
        // are no longer the ones in force.
        loaded(typeOf(i));
    }
}

void AccountDataHandler::worldEntered() {
    for (int i = 0; i < kAccountDataTypeCount; ++i) {
        if (slots_[i].unsent) send(typeOf(i));
    }
}

// ---- Queries ----------------------------------------------------------------

const std::string& AccountDataHandler::text(AccountDataType type) const {
    return slots_[static_cast<size_t>(type)].text;
}

const std::vector<Macro>& AccountDataHandler::macros(bool character) const {
    return macros_[character ? 1 : 0];
}

const Macro* AccountDataHandler::macro(uint32_t id) const {
    for (const auto& half : macros_) {
        for (const Macro& m : half) {
            if (m.id == id) return &m;
        }
    }
    return nullptr;
}

// ---- Commands ---------------------------------------------------------------

void AccountDataHandler::store(AccountDataType type, std::string text) {
    if (isCharacterAccountData(type) && characterGuid_ == 0) {
        LOG_WARNING("Account data: ", accountDataName(type),
                    " changed with no character logged in to own it; not kept");
        return;
    }
    Slot& slot = slots_[static_cast<size_t>(type)];
    slot.text = std::move(text);
    // The time of the change, which is what the server compares at the next
    // login - a later time here than there is how it learns of a change it
    // was not told about.
    slot.time = static_cast<uint32_t>(std::time(nullptr));
    writeCache(type);
    slot.unsent = true;
    send(type);
}

uint32_t AccountDataHandler::createMacro(bool character, const std::string& name,
                                         const std::string& icon, const std::string& body) {
    if (character && characterGuid_ == 0) {
        LOG_WARNING("Macro \"", name, "\" not created: no character is logged in to own it");
        return 0;
    }
    auto& half = macros_[character ? 1 : 0];
    const size_t limit = character ? kMaxCharacterMacros : kMaxAccountMacros;
    if (half.size() >= limit) {
        LOG_WARNING("Macro \"", name, "\" not created: the ", character ? "character's" : "account's",
                    " ", limit, " macros are all in use");
        return 0;
    }
    const uint32_t base = character ? kCharacterMacroIdBase : 0u;
    uint32_t id = base + 1;
    while (std::any_of(half.begin(), half.end(), [id](const Macro& m) { return m.id == id; })) ++id;
    half.push_back(Macro{id, name, iconName(icon), body});
    storeMacros(character);
    return id;
}

bool AccountDataHandler::editMacro(uint32_t id, const std::string* name, const std::string* icon,
                                   const std::string* body) {
    for (int c = 0; c < 2; ++c) {
        for (Macro& m : macros_[c]) {
            if (m.id != id) continue;
            if (name) m.name = *name;
            if (icon) m.icon = iconName(*icon);
            if (body) m.body = *body;
            storeMacros(c == 1);
            return true;
        }
    }
    return false;
}

bool AccountDataHandler::deleteMacro(uint32_t id) {
    for (int c = 0; c < 2; ++c) {
        auto& half = macros_[c];
        const auto it = std::find_if(half.begin(), half.end(), [id](const Macro& m) { return m.id == id; });
        if (it == half.end()) continue;
        half.erase(it);
        storeMacros(c == 1);
        events_.publish(events::MacroDeleted{id});
        return true;
    }
    return false;
}

void AccountDataHandler::adoptLegacyMacros(std::vector<Macro> macros) {
    for (Macro& m : macros) {
        const bool character = m.id >= kCharacterMacroIdBase;
        m.icon = iconName(m.icon);
        legacyMacros_[character ? 1 : 0].push_back(std::move(m));
    }
    for (int c = 0; c < 2; ++c) {
        if (mergeLegacyMacros(c == 1)) storeMacros(c == 1);
    }
}

// ---- Packets ----------------------------------------------------------------

void AccountDataHandler::handleAccountDataTimes(network::Packet& packet) {
    if (!serverKeepsAccountData()) {
        // Classic and TBC cores send a block of zeroed digests here and keep
        // nothing behind them; the disk copy is the account data.
        packet.skipAll();
        return;
    }
    AccountDataTimesData data;
    if (!AccountDataTimesParser::parse(packet, data)) return;  // said by the parser

    for (int i = 0; i < kAccountDataTypeCount; ++i) {
        if ((data.mask & (1u << i)) == 0) continue;
        const AccountDataType type = typeOf(i);
        if (!slotInScope(type)) {
            LOG_DEBUG("Account data: time for ", accountDataName(type),
                      " with no character logged in; not compared");
            continue;
        }
        Slot& slot = slots_[i];
        const uint32_t serverTime = data.accountDataTimes[i];
        if (serverTime > slot.time) {
            if (slot.requested) continue;
            slot.requested = true;
            LOG_INFO("Account data: asking for ", accountDataName(type), " (server ", serverTime,
                     ", here ", slot.time, ")");
            sender_.send(RequestAccountDataPacket::build(static_cast<uint32_t>(i)));
        } else if (serverTime < slot.time) {
            // Changed here and never reached the server - an edit made while
            // it could not be told, or a slot it has lost.
            LOG_INFO("Account data: sending ", accountDataName(type), " (here ", slot.time,
                     ", server ", serverTime, ")");
            slot.unsent = true;
            send(type);
        }
    }
}

void AccountDataHandler::handleUpdateAccountData(network::Packet& packet) {
    if (!serverKeepsAccountData()) {
        packet.skipAll();
        return;
    }
    UpdateAccountDataData data;
    std::string error;
    const bool parsed = UpdateAccountDataParser::parse(packet, data, error);
    if (data.type >= static_cast<uint32_t>(kAccountDataTypeCount)) {
        LOG_WARNING("SMSG_UPDATE_ACCOUNT_DATA: slot ", data.type, " is not one of the eight; ignored");
        return;
    }
    const AccountDataType type = typeOf(static_cast<int>(data.type));
    Slot& slot = slots_[data.type];
    slot.requested = false;
    if (!parsed) {
        warnOnce("download:" + std::to_string(data.type),
                 std::string("Account data: the server's ") + accountDataName(type) +
                 " could not be read (" + error + "); the copy on disk stays in use");
        return;
    }
    if (!slotInScope(type) ||
        (isCharacterAccountData(type) && data.guid != 0 && data.guid != characterGuid_)) {
        // An answer for the character before this one, arriving after the
        // switch. Taking it would put one character's macros on another.
        LOG_DEBUG("Account data: ", accountDataName(type), " for 0x", std::hex, data.guid, std::dec,
                  " is not the logged-in character's; ignored");
        return;
    }
    LOG_INFO("Account data: received ", accountDataName(type), " (", data.text.size(),
             " bytes, changed ", data.time, ")");
    slot.text = std::move(data.text);
    slot.time = data.time;
    slot.unsent = false;
    writeCache(type);
    loaded(type);
}

void AccountDataHandler::handleUpdateAccountDataComplete(network::Packet& packet) {
    if (!packet.hasRemaining(8)) {
        LOG_WARNING("SMSG_UPDATE_ACCOUNT_DATA_COMPLETE: ", packet.getSize(), " bytes, expected 8");
        packet.skipAll();
        return;
    }
    const uint32_t type = packet.readUInt32();
    const uint32_t result = packet.readUInt32();
    if (result != 0) {
        LOG_WARNING("Account data: the server refused slot ", type, " (result ", result,
                    "); it keeps its older copy and the next login sends this one again");
        return;
    }
    LOG_DEBUG("Account data: server stored slot ", type);
}

// ---- Internals --------------------------------------------------------------

bool AccountDataHandler::serverKeepsAccountData() const {
    return isActiveExpansion("wotlk");
}

bool AccountDataHandler::slotInScope(AccountDataType type) const {
    return !isCharacterAccountData(type) || characterGuid_ != 0;
}

std::string AccountDataHandler::cacheDir(AccountDataType type) const {
    if (cacheRoot_.empty() || accountKey_.empty()) return {};
    std::filesystem::path dir = std::filesystem::path(cacheRoot_) / accountKey_;
    if (isCharacterAccountData(type)) {
        std::ostringstream guid;
        guid << std::hex << std::setw(16) << std::setfill('0') << characterGuid_;
        dir /= guid.str();
    }
    return dir.string();
}

void AccountDataHandler::loadFromCache(AccountDataType type) {
    const std::string dir = cacheDir(type);
    if (dir.empty()) {
        warnOnce("nodir", "Account data: no directory to keep it in; settings, bindings and "
                          "macros last only as long as the session unless the server keeps them");
        return;
    }
    const std::filesystem::path path = std::filesystem::path(dir) / cacheFileName(type);
    std::ifstream in(path, std::ios::binary);
    if (!in) return;  // nothing kept yet
    std::string header;
    std::getline(in, header);
    if (header.compare(0, kCacheHeader.size(), kCacheHeader) != 0) {
        warnOnce("badcache:" + path.string(),
                 "Account data: " + path.string() + " does not start with its header; ignored, "
                 "and the server's copy is asked for instead");
        return;
    }
    Slot& slot = slots_[static_cast<size_t>(type)];
    try {
        slot.time = static_cast<uint32_t>(std::stoul(header.substr(kCacheHeader.size())));
    } catch (const std::exception&) {
        warnOnce("badcache:" + path.string(),
                 "Account data: " + path.string() + " has no time in its header; ignored");
        return;
    }
    std::ostringstream rest;
    rest << in.rdbuf();
    slot.text = rest.str();
}

void AccountDataHandler::writeCache(AccountDataType type) {
    const std::string dir = cacheDir(type);
    if (dir.empty()) return;  // said when reading
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = std::filesystem::path(dir) / cacheFileName(type);
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const Slot& slot = slots_[static_cast<size_t>(type)];
        out << kCacheHeader << slot.time << '\n' << slot.text;
        if (!out) {
            warnOnce("write:" + path.string(),
                     "Account data: could not write " + tmp.string() +
                     "; this change is kept for the session and sent, but not on disk");
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        warnOnce("write:" + path.string(),
                 "Account data: could not replace " + path.string() + " (" + ec.message() +
                 "); this change is kept for the session and sent, but not on disk");
    }
}

void AccountDataHandler::send(AccountDataType type) {
    Slot& slot = slots_[static_cast<size_t>(type)];
    if (!serverKeepsAccountData()) {
        slot.unsent = false;  // the disk copy is the only copy
        return;
    }
    if (!sender_.connected()) return;  // compared and sent at the next login
    // A character's slots are stored against the character the server has
    // loaded, so they wait until it has.
    if (isCharacterAccountData(type) && !sender_.characterInWorld()) return;
    std::string error;
    auto packet = UpdateAccountDataPacket::build(static_cast<uint32_t>(type), slot.time, slot.text, error);
    if (!packet) {
        warnOnce("upload:" + std::to_string(static_cast<int>(type)),
                 std::string("Account data: ") + accountDataName(type) + " not sent (" + error +
                 "); kept on disk only");
        slot.unsent = false;
        return;
    }
    sender_.send(*packet);
    slot.unsent = false;
}

void AccountDataHandler::loaded(AccountDataType type) {
    const Slot& slot = slots_[static_cast<size_t>(type)];
    if (type == AccountDataType::GlobalMacros || type == AccountDataType::CharacterMacros) {
        const bool character = type == AccountDataType::CharacterMacros;
        std::vector<std::string> problems;
        auto list = parseMacrosCache(slot.text, &problems);
        for (const std::string& problem : problems) {
            LOG_WARNING("Account data: ", accountDataName(type), ": ", problem,
                        "; that macro is not shown");
        }
        // A macro in the wrong half would be listed on one tab and sent back
        // to the other's slot; the id says which half it belongs to.
        std::erase_if(list, [&](const Macro& m) {
            if ((m.id >= kCharacterMacroIdBase) == character) return false;
            LOG_WARNING("Account data: macro ", m.id, " \"", m.name, "\" in ", accountDataName(type),
                        " has the other half's id; not shown");
            return true;
        });
        std::sort(list.begin(), list.end(), macroOrder);
        macros_[character ? 1 : 0] = std::move(list);
        if (mergeLegacyMacros(character)) {
            storeMacros(character);
        } else {
            frameXml_.fireFrameXmlEvent("UPDATE_MACROS", {});
            frameXml_.fireFrameXmlEvent("ACTIONBAR_SLOT_CHANGED", {"0"});
        }
    }
    events_.publish(events::AccountDataLoaded{static_cast<uint8_t>(type), slot.text});
}

void AccountDataHandler::storeMacros(bool character) {
    auto& half = macros_[character ? 1 : 0];
    std::sort(half.begin(), half.end(), macroOrder);
    store(macroSlot(character), writeMacrosCache(half));
    frameXml_.fireFrameXmlEvent("UPDATE_MACROS", {});
    frameXml_.fireFrameXmlEvent("ACTIONBAR_SLOT_CHANGED", {"0"});
}

bool AccountDataHandler::mergeLegacyMacros(bool character) {
    auto& legacy = legacyMacros_[character ? 1 : 0];
    if (legacy.empty() || (character && characterGuid_ == 0)) return false;
    auto& half = macros_[character ? 1 : 0];
    const size_t limit = character ? kMaxCharacterMacros : kMaxAccountMacros;
    bool added = false;
    for (const Macro& old : legacy) {
        if (std::any_of(half.begin(), half.end(), [&](const Macro& m) { return m.name == old.name; })) continue;
        if (half.size() >= limit) {
            LOG_WARNING("Macro \"", old.name, "\" from the old character file not kept: the ",
                        character ? "character's" : "account's", " macros are full");
            continue;
        }
        Macro m = old;
        const uint32_t base = character ? kCharacterMacroIdBase : 0u;
        // Its old id when free, so a button that held it still does.
        if (std::any_of(half.begin(), half.end(), [&](const Macro& h) { return h.id == m.id; })) {
            m.id = base + 1;
            while (std::any_of(half.begin(), half.end(), [&](const Macro& h) { return h.id == m.id; })) ++m.id;
        }
        LOG_INFO("Macro \"", m.name, "\" carried over from the old character file");
        half.push_back(std::move(m));
        added = true;
    }
    return added;
}

void AccountDataHandler::warnOnce(const std::string& key, const std::string& message) {
    if (!warned_.insert(key).second) return;
    LOG_WARNING(message);
}

} // namespace game
} // namespace wowee
