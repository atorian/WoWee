#pragma once

// A SpellVisualKit as the client plays it (WoW.exe 3.3.5a; render spec
// sections 3.5 and 6b): every effect column it fills is a model of its own,
// hung from the matching attachment of the unit it is played on and carried
// by that attachment's whole matrix every frame - bone, facing and scale
// (CM2Model::AttachToParent 0x831630, followed in 0x82e550). Only the World
// column stays where it was played.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::rendering {

/// One effect column of SpellVisualKit.dbc and where the client hangs it
/// (table at 0xa2d3f0): the head effect at Head, not Helm; the chest effect
/// at Chest, not ChestBloodFront.
struct SpellKitColumn {
    const char* field;       ///< the column's name in dbc_layouts.json
    uint32_t attachmentId;   ///< M2 attachment; kSpellKitWorldPlaced for World
};

/// The World column: placed at the position the kit was played at.
inline constexpr uint32_t kSpellKitWorldPlaced = 0xFFFFFFFFu;

inline constexpr SpellKitColumn kSpellKitColumns[] = {
    {.field = "HeadEffect",        .attachmentId = 20},  // Head
    {.field = "ChestEffect",       .attachmentId = 34},  // Chest
    {.field = "BaseEffect",        .attachmentId = 19},  // Base
    {.field = "LeftHandEffect",    .attachmentId = 21},  // SpellLeftHand
    {.field = "RightHandEffect",   .attachmentId = 22},  // SpellRightHand
    {.field = "BreathEffect",      .attachmentId = 17},  // Breath
    {.field = "LeftWeaponEffect",  .attachmentId = 2},   // HandLeft
    {.field = "RightWeaponEffect", .attachmentId = 1},   // HandRight
    {.field = "SpecialEffect0",    .attachmentId = 23},  // Special1
    {.field = "SpecialEffect1",    .attachmentId = 24},  // Special2
    {.field = "SpecialEffect2",    .attachmentId = 25},  // Special3
    {.field = "WorldEffect",       .attachmentId = kSpellKitWorldPlaced},
};

/// One model a kit plays: SpellVisualEffectName's file, scale and limits.
struct SpellKitEffect {
    std::string modelPath;
    uint32_t attachmentId = kSpellKitWorldPlaced;
    float scale = 1.0f;
    float minScale = 0.0f;   ///< 0: no limit
    float maxScale = 0.0f;   ///< 0: no limit
};

/// Every model a kit plays.
struct SpellVisualKit {
    std::vector<SpellKitEffect> effects;
};

/// The scale an effect is drawn at: its own, times what it hangs from, kept
/// within its limits (a limit of 0 is none).
inline float spellKitEffectScale(const SpellKitEffect& effect, float attachmentScale) {
    float s = effect.scale * attachmentScale;
    if (effect.minScale > 0.0f && s < effect.minScale) s = effect.minScale;
    if (effect.maxScale > 0.0f && s > effect.maxScale) s = effect.maxScale;
    return s;
}

} // namespace wowee::rendering
