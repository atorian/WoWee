#include "game/emote_table.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/game_data.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "foundation/logger.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace wowee {
namespace game {

namespace anim = rendering::anim;

// ── Helper functions (moved from animation_controller.cpp) ───────────────────

static std::vector<std::string> parseEmoteCommands(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool isLoopingEmote(const std::string& command) {
    static const std::unordered_set<std::string> kLooping = {
        "dance", "train", "dead", "eat", "work", "sleep",
    };
    return kLooping.find(command) != kLooping.end();
}

static std::string replacePlaceholders(const std::string& text, const std::string* targetName) {
    if (text.empty()) return text;
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == 's') {
            if (targetName && !targetName->empty()) out += *targetName;
            i++;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

// ── EmoteTable ───────────────────────────────────────────────────────────────

void EmoteTable::ensureLoaded() const {
    if (loaded_) return;
    loaded_ = true;

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* etdL = activeLayout ? activeLayout->getLayout("EmotesTextData") : nullptr;
    const auto* emL  = activeLayout ? activeLayout->getLayout("Emotes") : nullptr;
    const auto* etL  = activeLayout ? activeLayout->getLayout("EmotesText") : nullptr;
    // The widest column read of each, by the layout or the WotLK file's place.
    const auto widest = [](const pipeline::DBCFieldMap* layout,
                           std::initializer_list<std::pair<const char*, uint32_t>> columns) {
        uint32_t out = 0;
        for (const auto& [name, wotlk] : columns) {
            const uint32_t column = layout ? layout->field(name) : wotlk;
            if (column != 0xFFFFFFFF) out = std::max(out, column);
        }
        return out + 1;
    };
    constexpr const char* kNeed = "the emotes";
    const auto emotesTextDbc = data_.table(
        "EmotesText.dbc",
        widest(etL, {{"ID", 0}, {"Command", 1}, {"EmoteRef", 2}, {"OthersTargetTextID", 3},
                     {"SenderTargetTextID", 5}, {"OthersNoTargetTextID", 7}, {"SenderNoTargetTextID", 9}}),
        kNeed);
    const auto emotesTextDataDbc = data_.table("EmotesTextData.dbc",
                                               widest(etdL, {{"ID", 0}, {"Text", 1}}), kNeed);

    std::unordered_map<uint32_t, std::string> textData;
    textData.reserve(emotesTextDataDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDataDbc->getRecordCount(); ++r) {
        uint32_t id = emotesTextDataDbc->getUInt32(r, etdL ? (*etdL)["ID"] : 0);
        std::string text = emotesTextDataDbc->getString(r, etdL ? (*etdL)["Text"] : 1);
        if (!text.empty()) textData.emplace(id, std::move(text));
    }

    std::unordered_map<uint32_t, uint32_t> emoteIdToAnim;
    animByEmotesId_.clear();
    stateEmoteIds_.clear();
    {
        // EmoteSpecProc: 0 = one-shot, 1 = stand-state change, 2 = looping state
        // anim. Column 4 in every expansion's Emotes.dbc.
        uint32_t specField = emL ? (*emL)["EmoteSpecProc"] : 4;
        if (specField == 0xFFFFFFFF) specField = 4;
        const auto emotesDbc = data_.table(
            "Emotes.dbc", std::max(widest(emL, {{"ID", 0}, {"AnimID", 2}}), specField + 1), kNeed);
        emoteIdToAnim.reserve(emotesDbc->getRecordCount());
        animByEmotesId_.reserve(emotesDbc->getRecordCount());
        for (uint32_t r = 0; r < emotesDbc->getRecordCount(); ++r) {
            uint32_t emoteId = emotesDbc->getUInt32(r, emL ? (*emL)["ID"] : 0);
            uint32_t animId = emotesDbc->getUInt32(r, emL ? (*emL)["AnimID"] : 2);
            if (emotesDbc->getUInt32(r, specField) != 0) {
                stateEmoteIds_.insert(emoteId);
            }
            if (animId != 0) {
                emoteIdToAnim[emoteId] = animId;
                animByEmotesId_[emoteId] = animId;
            }
        }
        LOG_DEBUG("Emotes: loaded ", emoteIdToAnim.size(), " anim mappings from Emotes.dbc");
    }

    emoteTable_.clear();
    emoteTable_.reserve(emotesTextDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDbc->getRecordCount(); ++r) {
        uint32_t recordId = emotesTextDbc->getUInt32(r, etL ? (*etL)["ID"] : 0);
        std::string cmdRaw = emotesTextDbc->getString(r, etL ? (*etL)["Command"] : 1);
        if (cmdRaw.empty()) continue;

        uint32_t emoteRef = emotesTextDbc->getUInt32(r, etL ? (*etL)["EmoteRef"] : 2);
        uint32_t animId = 0;
        if (emoteRef != 0) {
            auto animIt = emoteIdToAnim.find(emoteRef);
            if (animIt != emoteIdToAnim.end()) {
                animId = animIt->second;
            }
            // If Emotes.dbc has AnimID=0 for this ref, leave animId=0 (text-only).
            // Previously fell back to using emoteRef as animId which is wrong.
        }

        uint32_t senderTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderTargetTextID"] : 5);
        uint32_t senderNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderNoTargetTextID"] : 9);
        uint32_t othersTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersTargetTextID"] : 3);
        uint32_t othersNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersNoTargetTextID"] : 7);

        std::string textTarget, textNoTarget, oTarget, oNoTarget;
        if (auto it = textData.find(senderTargetTextId); it != textData.end()) textTarget = it->second;
        if (auto it = textData.find(senderNoTargetTextId); it != textData.end()) textNoTarget = it->second;
        if (auto it = textData.find(othersTargetTextId); it != textData.end()) oTarget = it->second;
        if (auto it = textData.find(othersNoTargetTextId); it != textData.end()) oNoTarget = it->second;

        for (const std::string& cmd : parseEmoteCommands(cmdRaw)) {
            if (cmd.empty()) continue;
            EmoteInfo info;
            info.animId = animId;
            info.dbcId = recordId;
            info.loop = isLoopingEmote(cmd);
            info.textNoTarget = textNoTarget;
            info.textTarget = textTarget;
            info.othersNoTarget = oNoTarget;
            info.othersTarget = oTarget;
            info.command = cmd;
            emoteTable_.emplace(cmd, std::move(info));
        }
    }

    // Override emotes whose DBC chain yields animId=0.
    // /sleep uses the stand-state system in WoW rather than Emotes.dbc AnimID.
    // /laugh and /flirt should resolve from Emotes.dbc (70 and 83), but these
    // serve as backup if Emotes.dbc failed to load.
    // /fart and /stink have EmoteRef=0 in EmotesText.dbc - no Emotes.dbc link.
    static const std::unordered_map<std::string, uint32_t> kAnimOverrides = {
        {"sleep",   anim::EMOTE_SLEEP},     // 71 - stand-state emote
        {"laugh",   anim::EMOTE_LAUGH},     // 70 - backup
        {"flirt",   anim::EMOTE_SHY},       // 83 - DBC calls it SHY; it's the flirt animation
        {"fart",    anim::EMOTE_TALK},       // 60 - generic gesture (WoW has no dedicated anim)
        {"stink",   anim::EMOTE_TALK},       // 60 - generic gesture (WoW has no dedicated anim)
    };
    for (auto& [cmd, info] : emoteTable_) {
        if (info.animId == 0) {
            auto ov = kAnimOverrides.find(cmd);
            if (ov != kAnimOverrides.end()) {
                LOG_DEBUG("Emotes: override /", cmd, " → animId=", ov->second);
                info.animId = ov->second;
            }
        }
    }

    if (emoteTable_.empty()) {
        LOG_WARNING("Emotes: EmotesText.dbc has ", emotesTextDbc->getRecordCount(),
                    " rows and no command in any - no emote can be used");
    } else {
        LOG_DEBUG("Emotes: loaded ", emoteTable_.size(), " commands from DBC");
    }

    buildDbcIdIndex();
}

void EmoteTable::buildDbcIdIndex() const {
    emoteByDbcId_.clear();
    for (auto& [cmd, info] : emoteTable_) {
        if (info.dbcId != 0) {
            emoteByDbcId_.emplace(info.dbcId, &info);
        }
    }
}

std::optional<EmoteTable::EmoteResult> EmoteTable::findEmote(const std::string& command) const {
    ensureLoaded();
    auto it = emoteTable_.find(command);
    if (it == emoteTable_.end()) return std::nullopt;
    const auto& info = it->second;
    if (info.animId == 0) return std::nullopt;
    return EmoteResult{.animId = info.animId, .loop = info.loop};
}
uint32_t EmoteTable::animByEmotesId(uint32_t emoteId) const {
    ensureLoaded();
    auto it = animByEmotesId_.find(emoteId);
    return it != animByEmotesId_.end() ? it->second : 0;
}

bool EmoteTable::isStateEmote(uint32_t emoteId) const {
    ensureLoaded();
    return stateEmoteIds_.count(emoteId) != 0;
}

std::string EmoteTable::textFor(const std::string& emoteName,
                                const std::string* targetName) const {
    ensureLoaded();
    auto it = emoteTable_.find(emoteName);
    if (it != emoteTable_.end()) {
        const auto& info = it->second;
        const std::string& base = (targetName ? info.textTarget : info.textNoTarget);
        if (!base.empty()) {
            return replacePlaceholders(base, targetName);
        }
        if (targetName && !targetName->empty()) {
            return "You " + info.command + " at " + *targetName + ".";
        }
        return "You " + info.command + ".";
    }
    return "";
}

uint32_t EmoteTable::dbcIdFor(const std::string& emoteName) const {
    ensureLoaded();
    auto it = emoteTable_.find(emoteName);
    if (it != emoteTable_.end()) {
        return it->second.dbcId;
    }
    return 0;
}

std::string EmoteTable::textByDbcId(uint32_t dbcId,
                                    const std::string& senderName,
                                    const std::string* targetName) const {
    ensureLoaded();
    auto it = emoteByDbcId_.find(dbcId);
    if (it == emoteByDbcId_.end()) return "";

    const EmoteInfo& info = *it->second;

    if (targetName && !targetName->empty()) {
        if (!info.othersTarget.empty()) {
            std::string out;
            out.reserve(info.othersTarget.size() + senderName.size() + targetName->size());
            bool firstReplaced = false;
            for (size_t i = 0; i < info.othersTarget.size(); ++i) {
                if (info.othersTarget[i] == '%' && i + 1 < info.othersTarget.size() && info.othersTarget[i + 1] == 's') {
                    out += firstReplaced ? *targetName : senderName;
                    firstReplaced = true;
                    ++i;
                } else {
                    out.push_back(info.othersTarget[i]);
                }
            }
            return out;
        }
        return senderName + " " + info.command + "s at " + *targetName + ".";
    }         if (!info.othersNoTarget.empty()) {
            return replacePlaceholders(info.othersNoTarget, &senderName);
        }
        return senderName + " " + info.command + "s.";
   
}

} // namespace game
} // namespace wowee
