#pragma once

// The glow, sparks or flames an enchant puts on the weapon carrying it.
//
// The local player's weapons are dressed by AppearanceComposer and everyone
// else's by EntitySpawner, from different sources of the enchant id: the
// player's own item objects, and other players' visible-item fields. What the
// id then looks like is the same question for both, so it is answered here
// once. Before, only the composer asked it, and nobody else's weapon ever
// glowed.

#include "rendering/scene_commands.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace wowee {
namespace pipeline {
class AssetManager;
struct M2Model;
}

namespace core {

/// The models an enchant hangs on a weapon, one per ItemVisuals slot - the
/// slot is the attachment point on the weapon model it hangs from. Empty
/// where a slot has none; all empty for the many enchants that show nothing.
using EnchantVisualModels = std::array<std::string, 5>;

/// SpellItemEnchantment -> ItemVisuals -> ItemVisualEffects for one enchant
/// id. An id the tables do not have, or tables that are not there, are
/// reported once and answer nothing. Safe to call from a worker thread.
EnchantVisualModels resolveEnchantVisualModels(pipeline::AssetManager& assets, uint32_t enchantId);

/// Reads one model by path, the way the caller reads its weapons (the
/// spawner's come from a worker, the composer's from the archives).
using EnchantModelLoader = std::function<bool(const std::string& path, pipeline::M2Model& out)>;

/// Hang `enchantId`'s models on the weapon at `attachmentId` of the character
/// instance, in place of whatever enchant was shown there. Enchant 0 takes it
/// away. A model that does not load is reported once per path and the weapon
/// is drawn without it. `allocateModelId` names a fresh renderer model. The
/// effects go on at the scene's next apply point, after the weapon.
void attachEnchantVisuals(rendering::SceneCommands::Characters& characters, pipeline::AssetManager& assets,
                          uint32_t charInstanceId, uint32_t attachmentId, uint32_t enchantId,
                          const EnchantModelLoader& loadModel,
                          const std::function<uint32_t()>& allocateModelId);

} // namespace core
} // namespace wowee
