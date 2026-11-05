#include "ui/spellbook_screen.hpp"
#include "ui/ui_texture_load.hpp"
#include "ui/ui_colors.hpp"
#include "ui/keybinding_manager.hpp"
#include "platform/input.hpp"
#include "client/application.hpp"
#include "rendering/vk_context.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/required_table.hpp"
#include "foundation/logger.hpp"
#include "pipeline/spell_icon_paths.hpp"
#include <algorithm>
#include <map>
#include <cctype>

namespace wowee { namespace ui {

void SpellbookScreen::loadSpellDBC(pipeline::AssetManager* assetManager) {
    if (dbcLoadAttempted) return;
    // Not yet: no tables handed over, or no data mounted to read them from.
    if (!gameTables_ || !assetManager || !assetManager->isInitialized()) return;
    dbcLoadAttempted = true;

    constexpr std::string_view kNeededFor = "the spellbook";
    const pipeline::RequiredTable dbc(*gameTables_, "Spell.dbc", kNeededFor);

    // SpellCastTimes.dbc: ID, base cast time in ms.
    std::unordered_map<uint32_t, uint32_t> castTimeMap;  // index → base ms
    {
        const auto castTimeDbc = gameTables_->table("SpellCastTimes.dbc", 2, kNeededFor);
        for (uint32_t i = 0; i < castTimeDbc->getRecordCount(); ++i) {
            uint32_t id   = castTimeDbc->getUInt32(i, 0);
            int32_t  base = static_cast<int32_t>(castTimeDbc->getUInt32(i, 1));
            if (id > 0 && base > 0)
                castTimeMap[id] = static_cast<uint32_t>(base);
        }
    }

    // SpellRange.dbc: ID, then the hostile maximum where the expansion's
    // layout puts it (Classic has one maximum, TBC and WotLK two).
    std::unordered_map<uint32_t, float> rangeMap;  // index → max yards
    {
        const pipeline::RequiredTable rangeDbc(*gameTables_, "SpellRange.dbc", kNeededFor);
        const uint32_t maxRangeField = rangeDbc.column("MaxRange");
        for (uint32_t i = 0; i < rangeDbc->getRecordCount(); ++i) {
            uint32_t id = rangeDbc->getUInt32(i, 0);
            float maxRange = rangeDbc->getFloat(i, maxRangeField);
            if (id > 0 && maxRange > 0.0f)
                rangeMap[id] = maxRange;
        }
    }

    // TBC and WotLK keep a school bitmask, Classic a 0-6 school
    // number; each expansion's layout names the one its file has.
    uint32_t schoolField = dbc.optionalColumn("SchoolMask");
    const bool isSchoolEnum = schoolField == pipeline::RequiredTable::kAbsent;
    if (isSchoolEnum) schoolField = dbc.column("SchoolEnum");

    const uint32_t idField = dbc.column("ID");
    const uint32_t attrField = dbc.column("Attributes");
    const uint32_t iconField = dbc.column("IconID");
    const uint32_t nameField = dbc.column("Name");
    const uint32_t rankField = dbc.column("Rank");
    const uint32_t tooltipField = dbc.column("Tooltip");
    const uint32_t descriptionField = dbc.column("Description");
    const uint32_t powerTypeField = dbc.column("PowerType");
    const uint32_t manaCostField = dbc.column("ManaCost");
    const uint32_t rangeIndexField = dbc.column("RangeIndex");
    // From the file's own shape rather than the expansion's name for it:
    // Classic named this 15, which is RequiresSpellFocus.
    const uint32_t castTimeIndexField =
        pipeline::detectSpellTimingFields(dbc.get(), &dbc.layout()).castingTimeIndex;
    // WotLK's; the older files have no aura-state requirement.
    const uint32_t casterAuraStateField = dbc.optionalColumn("CasterAuraState");
    const uint32_t casterAuraStateNotField = dbc.optionalColumn("CasterAuraStateNot");
    constexpr uint32_t kAbsent = pipeline::RequiredTable::kAbsent;

    spellData.clear();
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t spellId = dbc->getUInt32(i, idField);
        if (spellId == 0) continue;

        SpellInfo info;
        info.spellId = spellId;
        info.attributes = dbc->getUInt32(i, attrField);
        info.iconId = dbc->getUInt32(i, iconField);
        info.name = dbc->getString(i, nameField);
        info.rank = dbc->getString(i, rankField);
        // Prefer the fuller Description (mentions e.g. the "well fed" food buff) and
        // fall back to the short Tooltip; $-tokens are resolved at render time.
        info.description = dbc->getString(i, descriptionField);
        if (info.description.empty()) info.description = dbc->getString(i, tooltipField);
        info.powerType = dbc->getUInt32(i, powerTypeField);
        info.manaCost  = dbc->getUInt32(i, manaCostField);
        if (casterAuraStateField != kAbsent)
            info.casterAuraState = dbc->getUInt32(i, casterAuraStateField);
        if (casterAuraStateNotField != kAbsent)
            info.casterAuraStateNot = dbc->getUInt32(i, casterAuraStateNotField);
        if (uint32_t ctIdx = dbc->getUInt32(i, castTimeIndexField); ctIdx > 0) {
            auto ctIt = castTimeMap.find(ctIdx);
            if (ctIt != castTimeMap.end()) info.castTimeMs = ctIt->second;
        }
        if (uint32_t rangeIdx = dbc->getUInt32(i, rangeIndexField); rangeIdx > 0) {
            auto rangeIt = rangeMap.find(rangeIdx);
            if (rangeIt != rangeMap.end()) info.rangeIndex = static_cast<uint32_t>(rangeIt->second);
        }
        const uint32_t raw = dbc->getUInt32(i, schoolField);
        // enum→mask: schoolEnum N maps to bit (1u << N), e.g. 0→1 (physical), 4→16 (frost).
        info.schoolMask = isSchoolEnum ? (raw <= 6 ? (1u << raw) : 0u) : raw;

        if (!info.name.empty()) {
            spellData[spellId] = std::move(info);
        }
    }
    LOG_INFO("Spellbook: Loaded ", spellData.size(), " spells from Spell.dbc");
    dbcLoaded = !spellData.empty();
}

bool SpellbookScreen::renderSpellInfoTooltip(uint32_t spellId, game::GameHandler& gameHandler,
                                              pipeline::AssetManager* assetManager) {
    if (!dbcLoadAttempted) loadSpellDBC(assetManager);
    const SpellInfo* info = getSpellInfo(spellId);
    if (!info) return false;
    renderSpellTooltip(info, gameHandler, /*showUsageHints=*/false);
    return true;
}

uint32_t SpellbookScreen::getSpellMaxRange(uint32_t spellId, pipeline::AssetManager* assetManager) {
    if (!dbcLoadAttempted) {
        loadSpellDBC(assetManager);
    }
    auto it = spellData.find(spellId);
    if (it != spellData.end()) return it->second.rangeIndex;
    return 0;
}

void SpellbookScreen::getSpellPowerInfo(uint32_t spellId, pipeline::AssetManager* assetManager,
                                        uint32_t& outCost, uint32_t& outPowerType) {
    outCost = 0;
    outPowerType = 0;
    if (!dbcLoadAttempted) {
        loadSpellDBC(assetManager);
    }
    auto it = spellData.find(spellId);
    if (it != spellData.end()) {
        outCost = it->second.manaCost;
        outPowerType = it->second.powerType;
    }
}

void SpellbookScreen::getSpellAuraStateInfo(uint32_t spellId, pipeline::AssetManager* assetManager,
                                             uint32_t& outRequired, uint32_t& outForbidden) {
    outRequired = 0;
    outForbidden = 0;
    if (!dbcLoadAttempted) {
        loadSpellDBC(assetManager);
    }
    auto it = spellData.find(spellId);
    if (it != spellData.end()) {
        outRequired = it->second.casterAuraState;
        outForbidden = it->second.casterAuraStateNot;
    }
}

const SpellInfo* SpellbookScreen::getSpellInfo(uint32_t spellId) const {
    auto it = spellData.find(spellId);
    return (it != spellData.end()) ? &it->second : nullptr;
}

void SpellbookScreen::renderSpellTooltip(const SpellInfo* info, game::GameHandler& gameHandler, bool showUsageHints) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(320.0f);

    // Spell name in yellow
    ImGui::TextColored(ui::colors::kYellow, "%s", info->name.c_str());

    // Rank in gray
    if (!info->rank.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ui::colors::kGray, "(%s)", info->rank.c_str());
    }

    // Passive indicator
    if (info->isPassive()) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Passive");
    }

    // Spell school - only show for non-physical schools (physical is the default/implicit)
    if (info->schoolMask != 0 && info->schoolMask != 1 /*physical*/) {
        struct SchoolEntry { uint32_t mask; const char* name; ImVec4 color; };
        static constexpr SchoolEntry kSchools[] = {
            { .mask = 2,  .name = "Holy",    .color = { 1.0f, 1.0f, 0.6f, 1.0f } },
            { .mask = 4,  .name = "Fire",    .color = { 1.0f, 0.5f, 0.1f, 1.0f } },
            { .mask = 8,  .name = "Nature",  .color = { 0.4f, 0.9f, 0.3f, 1.0f } },
            { .mask = 16, .name = "Frost",   .color = { 0.5f, 0.8f, 1.0f, 1.0f } },
            { .mask = 32, .name = "Shadow",  .color = { 0.7f, 0.4f, 1.0f, 1.0f } },
            { .mask = 64, .name = "Arcane",  .color = { 0.9f, 0.5f, 1.0f, 1.0f } },
        };
        bool first = true;
        for (const auto& s : kSchools) {
            if (info->schoolMask & s.mask) {
                if (!first) ImGui::SameLine(0, 0);
                if (first) {
                    ImGui::TextColored(s.color, "%s", s.name);
                    first = false;
                } else {
                    ImGui::SameLine(0, 2);
                    ImGui::TextColored(s.color, "/%s", s.name);
                }
            }
        }
    }

    // Resource cost + cast time on same row (WoW style)
    if (!info->isPassive()) {
        // Left: resource cost (with talent flat/pct modifier applied)
        char costBuf[64] = "";
        if (info->manaCost > 0) {
            // Was its own table with Focus at 4, which is Happiness - a spell
            // costing Focus said "Mana" and one costing Happiness said "Focus".
            const char* powerName = game::powerTypeName(info->powerType);
            if (!powerName) powerName = "Mana";
            // Apply SMSG_SET_FLAT/PCT_SPELL_MODIFIER Cost modifier (SpellModOp::Cost = 14)
            int32_t flatCost = gameHandler.spells().spellFlatMod(game::GameHandler::SpellModOp::Cost);
            int32_t pctCost  = gameHandler.spells().spellPctMod(game::GameHandler::SpellModOp::Cost);
            uint32_t displayCost = static_cast<uint32_t>(
                game::GameHandler::applySpellMod(static_cast<int32_t>(info->manaCost), flatCost, pctCost));
            std::snprintf(costBuf, sizeof(costBuf), "%u %s", displayCost, powerName);
        }

        // Right: cast time (with talent CastingTime modifier applied)
        char castBuf[32] = "";
        if (info->castTimeMs == 0) {
            std::snprintf(castBuf, sizeof(castBuf), "Instant cast");
        } else {
            // Apply SpellModOp::CastingTime (10) modifiers
            int32_t flatCT = gameHandler.spells().spellFlatMod(game::GameHandler::SpellModOp::CastingTime);
            int32_t pctCT  = gameHandler.spells().spellPctMod(game::GameHandler::SpellModOp::CastingTime);
            int32_t modCT  = game::GameHandler::applySpellMod(
                static_cast<int32_t>(info->castTimeMs), flatCT, pctCT);
            float secs = static_cast<float>(modCT) / 1000.0f;
            std::snprintf(castBuf, sizeof(castBuf), "%.1f sec cast", secs > 0.0f ? secs : 0.0f);
        }

        if (costBuf[0] || castBuf[0]) {
            float wrapW = 320.0f;
            if (costBuf[0] && castBuf[0]) {
                float castW = ImGui::CalcTextSize(castBuf).x;
                ImGui::Text("%s", costBuf);
                ImGui::SameLine(wrapW - castW);
                ImGui::Text("%s", castBuf);
            } else if (castBuf[0]) {
                ImGui::Text("%s", castBuf);
            } else {
                ImGui::Text("%s", costBuf);
            }
        }

        // Range
        if (info->rangeIndex > 0) {
            char rangeBuf[32];
            if (info->rangeIndex <= 5)
                std::snprintf(rangeBuf, sizeof(rangeBuf), "Melee range");
            else
                std::snprintf(rangeBuf, sizeof(rangeBuf), "%u yd range", info->rangeIndex);
            ImGui::Text("%s", rangeBuf);
        }
    }

    // Cooldown if active
    float cd = gameHandler.spells().getSpellCooldown(info->spellId);
    if (cd > 0.0f) {
        ImGui::TextColored(ui::colors::kRed, "Cooldown: %.1fs", cd);
    }

    // Description - resolve WoW $-tokens (e.g. "increased by $s1") to concrete values.
    if (!info->description.empty()) {
        ImGui::Spacing();
        std::string desc = gameHandler.formatSpellDescription(info->spellId, info->description);
        ImGui::TextWrapped("%s", desc.c_str());
    }

    // Usage hints - only shown when browsing the spellbook, not on action bar hover
    if (!info->isPassive() && showUsageHints) {
        ImGui::Spacing();
        ImGui::TextColored(ui::colors::kBrightGreen, "Drag to action bar");
        ImGui::TextColored(ui::colors::kBrightGreen, "Double-click to cast");
    }

    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

}} // namespace wowee::ui
