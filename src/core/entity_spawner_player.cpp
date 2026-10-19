#include "core/entity_spawner.hpp"
#include "core/helm_visual.hpp"
#include "core/enchant_visual.hpp"
#include "core/weapon_sheath.hpp"
#include "core/geoset_rules.hpp"
#include "core/character_paths.hpp"
#include "pipeline/char_sections.hpp"

// M2 attachment 11 is the helm. 0 is the shield mount, which is where head gear
// was going: it attached, reported success, and hung off the forearm.
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "audio/npc_voice_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/item_textures.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/transport_manager.hpp"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace core {

namespace {
// The bare geoset ids, the group arithmetic and the appearance key all live in
// core/geoset_rules.hpp. This file used to carry its own copy of the constants,
// and the copy went stale: it named 2002 as "the" bare feet and never learned
// about 2001, which is how an HD model that spells its feet the other way lost
// them here while keeping them in the portrait.

// The head of a character's bare geoset set: the body, the one scalp it wears,
// and its facial hair.
//
// Two places build a player's geosets - one for a player seen across the world,
// one for the equipped composition - and they had already drifted: only one of
// them knew about the second bare-feet id, and only one of them treated a zero
// facial variant as "none" rather than as geoset x00.
std::unordered_set<uint16_t> bareGeosetsFor(uint16_t scalp,
                                            const EntitySpawner::FacialHairGeosets* facial,
                                            uint8_t raceId) {
    // No row for this character: the first variant of each facial group, which
    // on most models is the absence of the feature.
    const uint16_t f100 = facial ? facial->geoset100 : 1;
    const uint16_t f200 = facial ? facial->geoset200 : 1;
    const uint16_t f300 = facial ? facial->geoset300 : 1;
    return bareCharacterGeosets(scalp, f100, f200, f300, raceId);
}

uint16_t selectHairScalpGeoset(const std::unordered_map<uint32_t, uint16_t>& hairGeosets,
                               uint8_t raceId,
                               uint8_t genderId,
                               uint8_t hairStyleId) {
    const uint32_t key = appearanceKey(raceId, genderId, hairStyleId);
    auto it = hairGeosets.find(key);
    if (it != hairGeosets.end() && it->second > 0) {
        return it->second;
    }
    return 1;
}
} // namespace

void EntitySpawner::spawnOnlinePlayer(uint64_t guid,
                                    uint8_t raceId,
                                    uint8_t genderId,
                                    uint32_t appearanceBytes,
                                    uint8_t facialFeatures,
                                    float x, float y, float z, float orientation) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;
    if (spawns_->playerInstances_.count(guid)) return;

    // Skip local player - already spawned as the main character
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        uint64_t activeGuid = gameHandler_->getActiveCharacterGuid();
        if ((localGuid != 0 && guid == localGuid) ||
            (activeGuid != 0 && guid == activeGuid) ||
            (spawns_->spawnedPlayerGuid_ != 0 && guid == spawns_->spawnedPlayerGuid_)) {
            return;
        }
    }
    auto* charRenderer = renderer_->getCharacterRenderer();

    // Base geometry model: cache by (race, gender)
    uint32_t cacheKey = (static_cast<uint32_t>(raceId) << 8) | static_cast<uint32_t>(genderId & 0xFF);
    uint32_t modelId = 0;
    auto itCache = assets_.playerModelCache_.find(cacheKey);
    if (itCache != assets_.playerModelCache_.end()) {
        modelId = itCache->second;
        if (!charRenderer->getModelData(modelId)) {
            LOG_WARNING("spawnOnlinePlayer: cached player model missing after world reload, reloading modelId=",
                        modelId, " race=", static_cast<int>(raceId),
                        " gender=", static_cast<int>(genderId));
            assets_.playerTextureSlotsByModelId_.erase(modelId);
            assets_.playerModelCache_.erase(itCache);
            modelId = 0;
        }
    }
    // This consumer never reads or parses a file. Its worker supplied the
    // immutable model and finished appearance atlas before publishing readiness.
    if (!spawns_->preparedPlayerSpawn_ || !spawns_->preparedPlayerSpawn_->model) return;
    if (modelId == 0) {
        modelId = assets_.nextPlayerModelId_++;
        if (!charRenderer->loadModel(*spawns_->preparedPlayerSpawn_->model, modelId)) return;
        assets_.playerModelCache_[cacheKey] = modelId;
    }

    // Determine texture slots once per model
    {
        auto [slotIt, inserted] = assets_.playerTextureSlotsByModelId_.try_emplace(modelId);
        if (inserted) {
            PlayerTextureSlots slots;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    uint32_t t = md->textures[ti].type;
                    if (t == 1 && slots.skin < 0) slots.skin = static_cast<int>(ti);
                    else if (t == 6 && slots.hair < 0) slots.hair = static_cast<int>(ti);
                    else if (t == 8 && slots.skinExtra < 0) slots.skinExtra = static_cast<int>(ti);
                }
            }
            slotIt->second = slots;
        }
    }

    // Create instance at server position
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    float renderYaw = orientation + glm::radians(90.0f);
    uint32_t instanceId = scene().characters().createInstance(modelId, renderPos, glm::vec3(0.0f, 0.0f, renderYaw), 1.0f);
    if (instanceId == 0) return;

    const AppearanceBytes look = unpackAppearanceBytes(appearanceBytes);
    const uint8_t hairStyleId = look.hairStyleId;
    const auto& bodySkinPath = spawns_->preparedPlayerSpawn_->bodySkinPath;
    const auto& skinExtraPath = spawns_->preparedPlayerSpawn_->skinExtraPath;
    const auto& hairTexturePath = spawns_->preparedPlayerSpawn_->hairTexturePath;
    const auto& faceLowerPath = spawns_->preparedPlayerSpawn_->faceLowerPath;
    const auto& faceUpperPath = spawns_->preparedPlayerSpawn_->faceUpperPath;
    const auto& underwearPaths = spawns_->preparedPlayerSpawn_->underwearPaths;
    const std::string pelvisPath = defaultPelvisPath(raceId, genderId);

    // Composite base skin + face + underwear overlays
    rendering::TextureRef compositeTex;
    {
        std::vector<std::string> layers;
        layers.push_back(bodySkinPath);
        if (!faceLowerPath.empty()) layers.push_back(faceLowerPath);
        if (!faceUpperPath.empty()) layers.push_back(faceUpperPath);
        for (const auto& up : underwearPaths) layers.push_back(up);
        if (layers.size() > 1) {
            compositeTex = charRenderer->compositeTextures(layers);
        } else {
            compositeTex = charRenderer->loadTexture(bodySkinPath);
        }
    }

    rendering::TextureRef hairTex;
    if (!hairTexturePath.empty()) {
        hairTex = charRenderer->loadTexture(hairTexturePath);
    }
    // Texture type 8 is Skin Extra: the head detail sheet an HD model draws its
    // ears, eyes and eyelashes from. CharSections names it in the skin row's
    // second texture, which the tables the game shipped leave blank - so on a
    // stock model this still falls through to the underwear art it always used.
    rendering::TextureRef skinExtraTex;
    if (!skinExtraPath.empty()) skinExtraTex = charRenderer->loadTexture(skinExtraPath);
    else if (!underwearPaths.empty()) skinExtraTex = charRenderer->loadTexture(underwearPaths[0]);
    else skinExtraTex = charRenderer->loadTexture(pelvisPath);

    const PlayerTextureSlots& slots = assets_.playerTextureSlotsByModelId_[modelId];
    if (slots.skin >= 0 && compositeTex) {
        scene().characters().setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skin), compositeTex);
    }
    if (slots.hair >= 0 && hairTex) {
        scene().characters().setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.hair), hairTex);
    }
    if (slots.skinExtra >= 0 && skinExtraTex) {
        scene().characters().setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skinExtra), skinExtraTex);
    }

    // Geosets: body + selected hair/facial hair. Do not enable every group-0
    // submesh; that activates all hair scalp variants at once.
    const uint16_t selectedHairScalp = selectHairScalpGeoset(assets_.hairGeosetMap_, raceId, genderId, hairStyleId);
    auto itFacial = assets_.facialHairGeosetMap_.find(appearanceKey(raceId, genderId, facialFeatures));
    std::unordered_set<uint16_t> activeGeosets = bareGeosetsFor(
        selectedHairScalp,
        itFacial != assets_.facialHairGeosetMap_.end() ? &itFacial->second : nullptr, raceId);
    // This one is drawn before equipment is known and wants the no-cloak panel;
    // the shared set leaves group 15 alone so the equipment pass can decide.
    activeGeosets.insert(kGeosetNoCape);
    scene().characters().setActiveGeosets(instanceId, activeGeosets);

    if (spawns_->deadCreatureGuids_.count(guid)) {
        scene().characters().playAnimation(instanceId, rendering::anim::DEATH, false);
    } else {
        scene().characters().playAnimation(instanceId, rendering::anim::STAND, true);
    }
    spawns_->playerInstances_[guid] = instanceId;

    // The mount field may have arrived before this render instance, or the
    // player may be re-created without another values update. Reconcile from
    // retained entity state so already-mounted players are never left on foot.
    if (gameHandler_) {
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
        if (unit && unit->getMountDisplayId() != 0) {
            setRemotePlayerMountDisplayId(guid, unit->getMountDisplayId());
        }
    }

    OnlinePlayerAppearanceState st;
    st.instanceId = instanceId;
    st.modelId = modelId;
    st.raceId = raceId;
    st.genderId = genderId;
    st.appearanceBytes = appearanceBytes;
    st.facialFeatures = facialFeatures;
    st.bodySkinPath = bodySkinPath;
    // Include face textures so compositeWithRegions can rebuild the full base
    if (!faceLowerPath.empty()) st.underwearPaths.push_back(faceLowerPath);
    if (!faceUpperPath.empty()) st.underwearPaths.push_back(faceUpperPath);
    for (const auto& up : underwearPaths) st.underwearPaths.push_back(up);
    spawns_->onlinePlayerAppearance_[guid] = std::move(st);
}

void EntitySpawner::setOnlinePlayerEquipment(uint64_t guid, const WornEquipment& equipment) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;
    const auto& displayInfoIds = equipment.displayInfoIds;
    const auto& inventoryTypes = equipment.inventoryTypes;

    // Skip local player - equipment handled by GameScreen::updateCharacterGeosets/Textures
    // via consumeOnlineEquipmentDirty(), which fires on the same server update.
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        if (localGuid != 0 && guid == localGuid) return;
    }

    // If the player isn't spawned yet, store equipment until spawn.
    auto appIt = spawns_->onlinePlayerAppearance_.find(guid);
    if (!spawns_->playerInstances_.count(guid) || appIt == spawns_->onlinePlayerAppearance_.end()) {
        spawns_->pendingOnlinePlayerEquipment_[guid] = equipment;
        return;
    }

    OnlinePlayerAppearanceState& st = appIt->second;

    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    if (st.instanceId == 0 || st.modelId == 0) return;

    if (st.bodySkinPath.empty()) {
        LOG_DEBUG("setOnlinePlayerEquipment: bodySkinPath empty for guid=0x", std::hex, guid, std::dec,
                    " instanceId=", st.instanceId, " - skipping equipment");
        return;
    }

    int nonZeroDisplay = 0;
    for (uint32_t d : displayInfoIds) if (d != 0) nonZeroDisplay++;
    LOG_DEBUG("setOnlinePlayerEquipment: guid=0x", std::hex, guid, std::dec,
                " instanceId=", st.instanceId, " nonZeroDisplayIds=", nonZeroDisplay,
                " head=", displayInfoIds[0], " chest=", displayInfoIds[4],
                " legs=", displayInfoIds[6], " mainhand=", displayInfoIds[15]);

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

    auto getGeosetGroup = [&](uint32_t displayInfoId, uint32_t fieldIdx) -> uint32_t {
        if (displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        return displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), fieldIdx);
    };

    auto findDisplayIdByInvType = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0 || displayInfoIds[s] == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return displayInfoIds[s];
            }
        }
        return 0;
    };

    auto hasInvType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return true;
            }
        }
        return false;
    };

    // --- Geosets ---
    // Mirror the same group-range logic as CharacterPreview::applyEquipment to
    // keep other-player rendering consistent with the local character preview.
    // Group 4 (4xx) = forearms/gloves, 5 (5xx) = shins/boots, 8 (8xx) = wrists/sleeves,
    // 13 (13xx) = legs/trousers.  Missing defaults caused the shin-mesh gap (status.md).
    uint8_t hairStyleId = static_cast<uint8_t>((st.appearanceBytes >> 16) & 0xFF);
    const uint16_t selectedHairScalp = selectHairScalpGeoset(assets_.hairGeosetMap_, st.raceId, st.genderId, hairStyleId);
    auto itFacial = assets_.facialHairGeosetMap_.find(
        appearanceKey(st.raceId, st.genderId, st.facialFeatures));
    // The same bare set as everywhere else. This built its own and had drifted:
    // it named ears 701 where the other three name 702, which is the variant
    // that has ears on it - so a player composed through this path lost them.
    std::unordered_set<uint16_t> geosets = bareGeosetsFor(
        selectedHairScalp,
        itFacial != assets_.facialHairGeosetMap_.end() ? &itFacial->second : nullptr, st.raceId);

    const uint32_t geosetGroup1Field = idiL ? (*idiL)["GeosetGroup1"] : 7;
    const uint32_t geosetGroup2Field = idiL ? (*idiL)["GeosetGroup2"] : 8;
    const uint32_t geosetGroup3Field = idiL ? (*idiL)["GeosetGroup3"] : 9;

    std::unordered_set<uint16_t> modelGeosets;
    if (const auto* modelData = charRenderer->getModelData(st.modelId)) {
        for (const auto& batch : modelData->batches) {
            modelGeosets.insert(batch.submeshId);
        }
    }

    auto eraseGroup = [&](uint16_t group) {
        for (auto it = geosets.begin(); it != geosets.end();) {
            if ((*it / 100) == group) {
                it = geosets.erase(it);
            } else {
                ++it;
            }
        }
    };

    // NOTE (2026-08-11, unverified): this does NOT use core::resolveGeoset,
    // and the character preview's identically-named lambda does.
    //
    // geoset_rules.hpp says of that rule "it lives here now, with a test, and
    // the call sites ask rather than decide". This call site still decides -
    // it takes an exact match or the caller's fallback and nothing else.
    //
    // Where the two differ: an *equipped* geoset (a real variant, not a bare
    // 401/501/801/1301, which both treat as none and never substitute) that
    // the model does not carry. The preview substitutes the lowest member of
    // that group and shows some armour mesh; this returns the bare default, or
    // nothing at all if the model lacks that too. So a race whose model is
    // missing a variant would look bare in the world and dressed on the
    // character screen.
    //
    // Deliberately not changed here. It is a difference in what gets drawn on
    // the character you play, in the same area as the unresolved bare-shin
    // width bug, and it wants someone looking at the screen - which is the one
    // thing this pass could not do.
    auto pickGeoset = [&](uint16_t preferred, uint16_t fallback) -> uint16_t {
        if (preferred != 0 && modelGeosets.count(preferred) > 0) return preferred;
        if (fallback != 0 && modelGeosets.count(fallback) > 0) return fallback;
        return 0;
    };

    // Find the lowest submesh ID in a group (e.g., group 5 → lowest 5xx).
    // Races like Gnome (no 501) and Tauren (only 505) need this fallback.
    auto lowestInGroup = [&](uint16_t group) -> uint16_t {
        uint16_t best = 0;
        for (uint16_t g : modelGeosets) {
            if (g / 100 == group && (best == 0 || g < best)) best = g;
        }
        return best;
    };

    // Per-group defaults - overridden below when equipment provides a geoset value.
    uint16_t geosetGloves  = pickGeoset(kGeosetBareForearms, kGeosetBareForearms);
    uint16_t geosetBoots   = pickGeoset(kGeosetBareShins, lowestInGroup(5));
    uint16_t geosetSleeves = pickGeoset(kGeosetBareSleeves, kGeosetBareSleeves);
    uint16_t geosetPants   = pickGeoset(kGeosetBarePants, kGeosetBarePants);

    auto columnsOf = [&](uint32_t did) {
        return ItemGeosetColumns{getGeosetGroup(did, geosetGroup1Field),
                                 getGeosetGroup(did, geosetGroup2Field),
                                 getGeosetGroup(did, geosetGroup3Field)};
    };

    // Chest/Shirt/Robe (invType 4,5,20) → wrist/sleeve group 8
    // The chest or robe first, the shirt only without one. Searched together,
    // the shirt's slot comes first and won: a robe worn over a shirt lost
    // its skirt and its sleeves to the shirt under it.
    uint32_t chestDid = findDisplayIdByInvType({5, 20});
    if (chestDid == 0) chestDid = findDisplayIdByInvType({4});
    {
        uint32_t gg1 = getGeosetGroup(chestDid, geosetGroup1Field);
        if (gg1 > 0) geosetSleeves = pickGeoset(equippedGeoset(equipment::kChestBare, gg1), kGeosetBareSleeves);
    }

    // Belt down: knee, shirt tail, thigh and trousers, from the chest and legs.
    const LowerBodyGeosets lower =
        lowerBodyGeosets(columnsOf(chestDid), columnsOf(findDisplayIdByInvType({7})));
    geosetPants = pickGeoset(lower.trousers, kGeosetBarePants);
    const uint16_t geosetKnees = pickGeoset(lower.knees, 0);
    const uint16_t geosetShirtTail = pickGeoset(lower.shirtTail, 0);
    const uint16_t geosetThighs = pickGeoset(lower.thighs, 0);

    // Feet/Boots (invType 8) → shin group 5
    {
        uint32_t did = findDisplayIdByInvType({8});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetBoots = pickGeoset(equippedGeoset(equipment::kBootsBare, gg1), lowestInGroup(5));
    }

    // Hands/Gloves (invType 10) → forearm group 4
    {
        uint32_t did = findDisplayIdByInvType({10});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetGloves = pickGeoset(equippedGeoset(equipment::kGlovesBare, gg1), kGeosetBareForearms);
    }

    // Wrists/Bracers (invType 9) → sleeve group 8 (only if chest/shirt didn't set it)
    {
        uint32_t did = findDisplayIdByInvType({9});
        if (did != 0 && geosetSleeves == kGeosetBareSleeves) {
            uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
            if (gg1 > 0) geosetSleeves = pickGeoset(equippedGeoset(equipment::kChestBare, gg1), kGeosetBareSleeves);
        }
    }

    // Waist/Belt (invType 6) → buckle group 18
    //
    // The base variant when no belt is worn, rather than nothing. Group 18 is
    // erased below, and on the Legion human male 1801 is the waist itself, so
    // dropping it with nothing in its place left a gap between the torso and
    // the legs. The older models carry no 1801, where pickGeoset resolves this
    // to nothing exactly as before.
    uint16_t geosetBelt = 0;
    {
        uint32_t did = findDisplayIdByInvType({6});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        geosetBelt = pickGeoset(beltGeoset(gg1), equipment::kBeltBase);
    }

    eraseGroup(4);
    eraseGroup(5);
    eraseGroup(8);
    eraseGroup(9);
    eraseGroup(10);
    eraseGroup(11);
    eraseGroup(13);
    eraseGroup(15);
    eraseGroup(18);
    if (geosetGloves != 0) geosets.insert(geosetGloves);
    if (geosetBoots != 0) geosets.insert(geosetBoots);
    if (geosetSleeves != 0) geosets.insert(geosetSleeves);
    if (geosetPants != 0) geosets.insert(geosetPants);
    if (geosetKnees != 0) geosets.insert(geosetKnees);
    if (geosetShirtTail != 0) geosets.insert(geosetShirtTail);
    if (geosetThighs != 0) geosets.insert(geosetThighs);
    if (geosetBelt != 0) geosets.insert(geosetBelt);
    // Back/Cloak (invType 16)
    uint32_t capeGG = getGeosetGroup(findDisplayIdByInvType({16}), geosetGroup1Field);
    uint16_t geosetCape = pickGeoset(
        hasInvType({16}) ? cloakGeoset(capeGG) : kGeosetNoCape,
        kGeosetNoCape);
    if (geosetCape != 0) geosets.insert(geosetCape);
    // Tabard (invType 19) -> the flap, group 12. It asked for 1201 outright,
    // which is no flap, so no other player's tabard was ever drawn.
    eraseGroup(12);
    const uint16_t geosetTabard =
        pickGeoset(tabardGeoset(getGeosetGroup(findDisplayIdByInvType({19}), geosetGroup1Field)), 0);
    if (geosetTabard != 0) geosets.insert(geosetTabard);

    // Hide hair under helmets: replace style-specific scalp with bald scalp
    // HEAD slot is index 0 in the 19-element equipment array
    if (displayInfoIds[0] != 0 && hairStyleId > 0 &&
        core::helmHidesHair(*assetManager_, displayInfoIds[0], st.genderId)) {
        geosets.erase(selectedHairScalp);                              // Remove style scalp
        geosets.insert(1);    // Bald scalp cap (group 0)
    }

    scene().characters().setActiveGeosets(st.instanceId, geosets);

    // --- Helmet model attachment ---
    // HEAD slot is index 0 in the 19-element equipment array.
    // Helmet M2s are race/gender-specific (e.g. Helm_Plate_B_01_HuM.m2 for Human Male).
    if (displayInfoIds[0] != 0) {
        // Only the helm point - detaching 0 as well would drop the shield.
        scene().characters().detachWeapon(st.instanceId, kAttachHelm);

        const core::HelmVisual helm = core::resolveHelmVisual(
            *assetManager_, displayInfoIds[0], st.raceId, st.genderId);
        if (helm.valid()) {
            pipeline::M2Model helmModel;
            std::string helmPath;
            if (!helm.racialModelPath.empty()) {
                helmPath = helm.racialModelPath;
                if (!loadWeaponM2(helmPath, helmModel)) helmModel = {};
            }
            if (!helmModel.isValid()) {
                helmPath = helm.baseModelPath;
                loadWeaponM2(helmPath, helmModel);
            }

            if (helmModel.isValid()) {
                const uint32_t helmModelId = assets_.nextWeaponModelId_++;
                // Attachment point 0 (head bone), fallback to 11 (explicit head).
                const bool attached = scene().characters().attachWeapon(
                    st.instanceId, kAttachHelm, helmModel, helmModelId, helm.texturePath);
                if (attached) {
                    LOG_DEBUG("Attached player helmet: ", helmPath, " tex: ", helm.texturePath);
                }
            }
        }
    } else {
        // No helmet equipped - detach any existing helmet model
        scene().characters().detachWeapon(st.instanceId, kAttachHelm);
    }

    // --- Shoulder model attachment ---
    // SHOULDERS slot is index 2 in the 19-element equipment array.
    scene().characters().detachWeapon(st.instanceId, kAttachShoulderRight);
    scene().characters().detachWeapon(st.instanceId, kAttachShoulderLeft);
    if (displayInfoIds[2] != 0) {
        const ShoulderVisual shoulders =
            resolveShoulderVisual(*assetManager_, displayInfoIds[2], st.raceId, st.genderId);
        auto attachSide = [&](const HelmVisual& side, uint32_t attachmentId) {
            if (!side.valid()) return;
            pipeline::M2Model model;
            if (side.racialModelPath.empty() || !loadWeaponM2(side.racialModelPath, model)) model = {};
            if (!model.isValid() && !loadWeaponM2(side.baseModelPath, model)) return;
            if (scene().characters().attachWeapon(st.instanceId, attachmentId, model,
                                           assets_.nextWeaponModelId_++, side.texturePath)) {
                LOG_DEBUG("Attached shoulder ", attachmentId, ": ", side.baseModelPath,
                          " tex: ", side.texturePath);
            }
        };
        attachSide(shoulders.left, kAttachShoulderLeft);
        attachSide(shoulders.right, kAttachShoulderRight);
    }

    // --- Cape texture (group 15 / texture type 2) ---
    // The geoset above enables the cape mesh, but without a texture it renders blank.
    if (hasInvType({16})) {
        // Back/cloak is WoW equipment slot 14 (BACK) in the 19-element array.
        uint32_t capeDid = displayInfoIds[14];
        if (capeDid != 0) {
            int32_t capeRecIdx = displayInfoDbc->findRecordById(capeDid);
            if (capeRecIdx >= 0) {
                const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                // RightModelTexture is the field right after LeftModelTexture.
                // Some cloaks (e.g. Jaina's Radiance) carry their texture only in
                // the right field; the character-preview screen checks both, so
                // match it here - otherwise the world model shows the cape mesh
                // untextured even though the paperdoll preview looks correct.
                const uint32_t rightTexField = leftTexField + 1;
                std::string leftName = displayInfoDbc->getString(
                    static_cast<uint32_t>(capeRecIdx), leftTexField);
                std::string rightName = displayInfoDbc->getString(
                    static_cast<uint32_t>(capeRecIdx), rightTexField);

                std::vector<std::string> capeNames;
                auto addCapeName = [&](const std::string& n) {
                    if (!n.empty() &&
                        std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end())
                        capeNames.push_back(n);
                };
                if (st.genderId == 1) { addCapeName(rightName); addCapeName(leftName); }
                else                  { addCapeName(leftName);  addCapeName(rightName); }

                if (!capeNames.empty()) {
                    // Where a cape's art might be, in the order to try it -
                    // pipeline/item_textures.hpp. Written out here, in the NPC
                    // path and in the portrait, identically, which is the only
                    // reason the three agreed.
                    std::vector<std::string> capeCandidates;
                    for (const auto& capeName : capeNames) {
                        for (auto& c : pipeline::capeTextureCandidates(capeName, st.genderId == 1)) {
                            if (std::find(capeCandidates.begin(), capeCandidates.end(), c) ==
                                capeCandidates.end()) {
                                capeCandidates.push_back(std::move(c));
                            }
                        }
                    }

                    rendering::TextureRef capeTexture;
                    for (const auto& candidate : capeCandidates) {
                        rendering::TextureRef tex = charRenderer->loadTexture(candidate);
                        if (tex && tex != charRenderer->whiteTexture()) {
                            capeTexture = std::move(tex);
                            break;
                        }
                    }

                    if (capeTexture) {
                        scene().characters().setGroupTextureOverride(st.instanceId, 15, capeTexture);
                        if (const auto* md = charRenderer->getModelData(st.modelId)) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 2) {
                                    scene().characters().setTextureSlotOverride(
                                        st.instanceId, static_cast<uint16_t>(ti), capeTexture);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Textures (skin atlas compositing) ---

    if (!spawns_->preparedEquipmentUpdate_) return;
    const auto& regionLayers = spawns_->preparedEquipmentUpdate_->regions;

    const auto slotsIt = assets_.playerTextureSlotsByModelId_.find(st.modelId);
    if (slotsIt == assets_.playerTextureSlotsByModelId_.end()) return;
    const PlayerTextureSlots& slots = slotsIt->second;
    if (slots.skin < 0) return;

    rendering::TextureRef newTex = charRenderer->compositeWithRegions(st.bodySkinPath, st.underwearPaths, regionLayers);
    if (newTex) {
        scene().characters().setTextureSlotOverride(st.instanceId, static_cast<uint16_t>(slots.skin), newTex);
    }

    // --- Weapon model attachment ---
    // The main and off hand (slots 15 and 16 of the 19), in the hands - or,
    // on a mount, on the back, whatever their sheath state says. Every point
    // a weapon can be on is cleared first, so one that moved leaves no copy.
    for (const uint32_t point : kWeaponAttachmentPoints) scene().characters().detachWeapon(st.instanceId, point);
    const bool sheathed = spawns_->remotePlayerMounts_.count(guid) != 0;
    st.weaponsSheathed = sheathed;
    static constexpr std::pair<int, game::EquipSlot> weaponSlots[] = {
        {15, game::EquipSlot::MAIN_HAND},
        {16, game::EquipSlot::OFF_HAND},
    };

    for (size_t hand = 0; hand < 2; ++hand) {
        const auto [slotIndex, equipSlot] = weaponSlots[hand];
        st.weapons[hand] = {};
        const uint32_t weapDisplayId = displayInfoIds[slotIndex];
        if (weapDisplayId == 0) continue;

        int32_t recIdx = displayInfoDbc->findRecordById(weapDisplayId);
        if (recIdx < 0) {
            LOG_WARNING("Player 0x", std::hex, guid, std::dec, " weapon display ", weapDisplayId,
                        " is not in ItemDisplayInfo; drawn without it");
            continue;
        }

        const auto art = pipeline::readItemDisplayArt(*displayInfoDbc,
                                                      static_cast<uint32_t>(recIdx));
        if (art.modelFile.empty()) continue;  // an item with no model of its own
        const std::string& modelFile = art.modelFile;
        const std::string& textureName = art.textureName;

        // In the Weapon directory, or for a shield the Shield one. Chosen by
        // which holds the file, so the loader is asked for that one only and
        // a shield does not report its absence from the Weapon directory.
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        if (!assetManager_->fileExists(m2Path)) {
            const std::string shieldPath = "Item\\ObjectComponents\\Shield\\" + modelFile;
            if (assetManager_->fileExists(shieldPath)) m2Path = shieldPath;
        }
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) {
            LOG_WARNING("Player 0x", std::hex, guid, std::dec, " weapon display ", weapDisplayId,
                        ": model ", m2Path, " not loaded; drawn without it");
            continue;
        }

        // Build texture path
        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager_->fileExists(texturePath)) {
                texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
                if (!assetManager_->fileExists(texturePath)) texturePath.clear();
            }
        }

        const uint8_t inventoryType = inventoryTypes[slotIndex];
        const uint32_t point = sheathed ? riderWeaponAttachment(equipSlot, inventoryType)
                                        : weaponAttachment(false, equipSlot, inventoryType);
        uint32_t weaponModelId = assets_.nextWeaponModelId_++;
        if (!scene().characters().attachWeapon(st.instanceId, point, weaponModel, weaponModelId, texturePath)) {
            continue;  // attachWeapon says why
        }
        st.weapons[hand] = {.attached = true, .inventoryType = inventoryType};
        // The enchant it shows, the way the player's own weapons get theirs.
        attachEnchantVisuals(
            scene().characters(), *assetManager_, st.instanceId, point, equipment.enchantIds[slotIndex],
            [this](const std::string& path, pipeline::M2Model& model) { return loadWeaponM2(path, model); },
            [this] { return assets_.nextWeaponModelId_++; });
    }
}

void EntitySpawner::setRemotePlayerWeaponsSheathed(uint64_t guid, bool sheathed) {
    auto appIt = spawns_->onlinePlayerAppearance_.find(guid);
    if (appIt == spawns_->onlinePlayerAppearance_.end() || !renderer_) return;
    OnlinePlayerAppearanceState& st = appIt->second;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer || st.instanceId == 0 || st.weaponsSheathed == sheathed) return;

    static constexpr game::EquipSlot kHands[] = {game::EquipSlot::MAIN_HAND, game::EquipSlot::OFF_HAND};
    for (size_t hand = 0; hand < 2; ++hand) {
        if (!st.weapons[hand].attached) continue;
        const uint8_t inventoryType = st.weapons[hand].inventoryType;
        auto placed = [&](bool onBack) {
            return onBack ? riderWeaponAttachment(kHands[hand], inventoryType)
                          : weaponAttachment(false, kHands[hand], inventoryType);
        };
        scene().characters().moveWeapon(st.instanceId, placed(st.weaponsSheathed), placed(sheathed),
                                 glm::mat4(1.0f));
    }
    st.weaponsSheathed = sheathed;
}

/// Brings each visible player's drawn form in line with their unit.
///
/// The form is state the unit already holds (Unit::getFormDisplayId), so it is
/// read rather than announced: whichever order the display and native IDs
/// arrive in, a player who came into view mid-shift, and one re-created
/// without a new values update all come out the same way.
void EntitySpawner::reconcileRemotePlayerForms() {
    if (!gameHandler_) return;
    auto& entities = gameHandler_->getEntityManager();
    for (const auto& [guid, instanceId] : spawns_->playerInstances_) {
        (void)instanceId;
        auto entity = entities.getEntity(guid);
        if (!entity || entity->getType() != game::ObjectType::PLAYER) continue;
        uint32_t form = std::static_pointer_cast<game::Unit>(entity)->getFormDisplayId();
        if (form != 0 && assets_.undrawableFormDisplayIds_.count(form)) form = 0;
        // A humanoid display - an illusion or a costume into another race - is
        // a character model dressed from CreatureDisplayInfoExtra, which only
        // the NPC compositing path can do. Until forms are built by that path
        // the player keeps their own body rather than a bald, untextured one.
        if (form != 0) {
            auto disp = assets_.displayDataMap_.find(form);
            if (disp != assets_.displayDataMap_.end() && disp->second.extraDisplayId != 0) {
                assets_.undrawableFormDisplayIds_.insert(form);
                LOG_WARNING("Remote player humanoid form displayId=", form,
                         " is not drawn yet; the player keeps their own body");
                form = 0;
            }
        }
        setRemotePlayerFormDisplayId(guid, form);
    }
}

void EntitySpawner::despawnPlayer(uint64_t guid) {
    if (!spawns_) return;
    // Held across a reconnect: the game forgot it with the lost connection,
    // not because it left.
    if (spawns_->held_.count(guid)) return;
    spawns_->pendingPlayerSpawnGuids_.erase(guid);
    spawns_->jobs_.cancel(JobGroup::Channel::Player, guid);
    spawns_->jobs_.cancel(JobGroup::Channel::Equipment, guid);
    spawns_->pendingOnlinePlayerEquipment_.erase(guid);
    std::erase_if(spawns_->pendingPlayerSpawns_, [guid](const auto& request) { return request.guid == guid; });
    std::erase_if(spawns_->deferredEquipmentQueue_, [guid](const auto& request) { return request.first == guid; });
    if (!renderer_ || !renderer_->getCharacterRenderer()) return;
    spawns_->pendingRemotePlayerMounts_.erase(guid);
    removeRemotePlayerMount(guid);
    // Before the lookup below: what is freed with the model is the character,
    // and while a form is shown the map holds the form instead.
    spawns_->pendingRemotePlayerForms_.erase(guid);
    removeRemotePlayerForm(guid);
    auto it = spawns_->playerInstances_.find(guid);
    if (it == spawns_->playerInstances_.end()) return;
    // Only the instance goes. Its model is the race and gender's body, shared
    // by every player of that kind and kept in playerModelCache_ for the next
    // one; what makes this player look like itself (skin, hair, face,
    // equipment) lives on the instance.
    scene().characters().removeInstance(it->second);
    spawns_->playerInstances_.erase(it);
    spawns_->onlinePlayerAppearance_.erase(guid);
    spawns_->pendingOnlinePlayerEquipment_.erase(guid);
    spawns_->deadCreatureGuids_.erase(guid);
    spawns_->creatureRenderPosCache_.erase(guid);
    spawns_->creatureSwimmingState_.erase(guid);
    spawns_->creatureWalkingState_.erase(guid);
    spawns_->creatureFlyingState_.erase(guid);
    spawns_->creatureWasMoving_.erase(guid);
    spawns_->creatureWasSwimming_.erase(guid);
    spawns_->creatureWasFlying_.erase(guid);
    spawns_->creatureWasWalking_.erase(guid);
    spawns_->remotePlayerLocomotion_.erase(guid);
}

// ---------------------------------------------------------------------------
// Which hull a transport is drawn with
//
// Entry and displayId are different numbering spaces and this table used to mix
// them, which is how an elevator became a zeppelin. Every ship and zeppelin
// entry here shares one of five displayIds -- 3015, 3031, 7087, 7446, 7546 --
// and the numbers that were being compared against displayId were entries:
// 164871, 175080 and 176495 are the three vanilla zeppelins, and no displayId
// reaches six figures, so those three could never match.
//
// What did match was worse. 807 and 808 are the displayIds of Gnomeregan lifts
// (the "Vator" and the "Plunger"), 2454 belongs to the Searing Gorge scaffold
// cars and 1587 to a GameObject named, plainly, "Elevator" -- so every one of
// them was being drawn as an airship. Elevators are transports too, which is
// why the caller's guard lets them in.
//
// Values verified against gameobject_template.
std::string EntitySpawner::transportModelPath(uint32_t entry, uint32_t displayId) {
    struct TransportModel { uint32_t entry; uint32_t displayId; const char* path; };
    static constexpr const char* kShip     = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
    static constexpr const char* kZeppelin = "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo";
    static constexpr const char* kHordeZep = "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo";
    static constexpr const char* kIceship  = "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo";
    // NOLINTBEGIN(modernize-use-designated-initializers) - a table whose
    // columns are its field names, with the struct in view directly above.
    static constexpr TransportModel kTransportModels[] = {
        // Ships (display 3015)
        {  20808, 3015, kShip },      // The Maiden's Fancy
        { 176231, 3015, kShip },      // The Lady Mehley
        { 176310, 3015, kShip },      // The Bravery
        // Zeppelins (display 3031)
        { 164871, 3031, kZeppelin },  // The Thundercaller
        { 175080, 3031, kZeppelin },  // The Iron Eagle
        { 176495, 3031, kZeppelin },  // The Purple Princess
        { 186371, 3031, kZeppelin },
        { 190549, 3031, kZeppelin },  // The Zephyr
        // Horde zeppelins (display 7546)
        { 181689, 7546, kHordeZep },  // Cloudkisser
        { 186238, 7546, kHordeZep },  // The Mighty Wind
        { 201834, 7546, kHordeZep },
        // Icebreakers (display 7446)
        { 181688, 7446, kIceship },   // Northspear
        { 190536, 7446, kIceship },   // Stormwind's Pride
    };
    // NOLINTEND(modernize-use-designated-initializers)

    for (const TransportModel& t : kTransportModels) {
        if (entry == t.entry || displayId == t.displayId) return t.path;
    }
    // The Deeprun Tram car, which is an M2 rather than a WMO and is keyed on a
    // displayId that is genuinely a displayId: entries 176080-176086 all carry
    // 3831.
    if (displayId == 3831) {
        return "World\\Generic\\Gnome\\Passive Doodads\\Subway\\SubwayCar.m2";
    }
    return "";
}

void EntitySpawner::spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer_ || !assetManager_) return;

    if (!assets_.gameObjectLookupsBuilt_) {
        buildGameObjectDisplayLookups();
    }
    if (!assets_.gameObjectLookupsBuilt_) return;

    LOG_DEBUG("GO spawn attempt: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " entry=", entry,
             " pos=(", x, ", ", y, ", ", z, ")");

    auto goIt = spawns_->gameObjectInstances_.find(guid);
    if (goIt != spawns_->gameObjectInstances_.end()) {
        // A tracked instance ID is only meaningful while the renderer still holds
        // it. Renderer-wide clears (map change, device reset) drop instances
        // without going through despawnGameObject(), which used to leave this map
        // pointing at a dead handle - every later server CREATE for that GUID then
        // took the position-update path below and the object stayed invisible for
        // the rest of the session. Treat a dead handle as "not spawned".
        // One created earlier this frame is alive too, though not made yet.
        bool instanceAlive = false;
        if (renderer_) {
            instanceAlive = goIt->second.isWmo
                ? scene().worldModels().hasInstance(goIt->second.instanceId)
                : scene().models().hasInstance(goIt->second.instanceId);
        }
        if (!instanceAlive) {
            LOG_WARNING("GO render instance vanished - respawning: guid=0x", std::hex, guid, std::dec,
                        " displayId=", displayId, " instanceId=", goIt->second.instanceId);
            spawns_->gameObjectInstances_.erase(goIt);
            goIt = spawns_->gameObjectInstances_.end();
        }
    }
    if (goIt != spawns_->gameObjectInstances_.end()) {
        if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
            if (auto* transportManager = gameHandler_->getTransportManager()) {
                if (transportManager->getTransport(guid)) {
                    transportManager->rebindTransportInstance(
                        guid, goIt->second.instanceId, !goIt->second.isWmo, displayId);
                    transportManager->updateServerTransport(
                        guid, glm::vec3(x, y, z), orientation);
                } else {
                    gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
                }
            } else {
                gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }
            return;
        }

        // Already have a render instance - update its position (e.g. transport re-creation)
        auto& info = goIt->second;
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        LOG_DEBUG("GameObject position update: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
                 " pos=(", x, ", ", y, ", ", z, ")");
        if (renderer_) {
            glm::mat4 transform(1.0f);
            transform = glm::translate(transform, renderPos);
            if (info.isWmo) {
                transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                scene().worldModels().setInstanceTransform(info.instanceId, transform);
            } else {
                scene().models().setInstanceTransform(info.instanceId, transform);
            }
        }
        return;
    }

    std::string modelPath;

    if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
        modelPath = transportModelPath(entry, displayId);
        if (!modelPath.empty()) {
            LOG_INFO("Transport entry/display ", entry, "/", displayId, " → ", modelPath);
        }
    }

    // Fallback to normal displayId lookup if not a transport or no override matched
    if (modelPath.empty()) {
        modelPath = getGameObjectModelPathForDisplayId(displayId);
    }

    if (modelPath.empty()) {
        LOG_WARNING("No model path for gameobject displayId ", displayId, " (guid 0x", std::hex, guid, std::dec, ")");
        return;
    }

    // Log spawns to help debug duplicate objects (e.g., cathedral issue)
    LOG_DEBUG("GameObject spawn: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
             " model=", modelPath, " pos=(", x, ", ", y, ", ", z, ")");

    std::string lowerPath = modelPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool isWmo = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".wmo";

    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    const float renderYawWmo = orientation;
    // M2 game objects: model default faces +renderX. renderYaw = canonical + 90° = server_yaw
    // (same offset as creature/character renderer_ so all M2 models face consistently)
    const float renderYawM2go = orientation + glm::radians(90.0f);

    bool loadedAsWmo = false;
    if (isWmo) {
        auto* wmoRenderer = renderer_->getWMORenderer();
        if (!wmoRenderer) return;

        uint32_t modelId = 0;
        auto itCache = assets_.gameObjectDisplayIdWmoCache_.find(displayId);
        if (itCache != assets_.gameObjectDisplayIdWmoCache_.end()) {
            modelId = itCache->second;
            // Only use cached entry if the model is still resident in the renderer_
            if (wmoRenderer->isModelLoaded(modelId)) {
                loadedAsWmo = true;
            } else {
                assets_.gameObjectDisplayIdWmoCache_.erase(itCache);
                modelId = 0;
            }
        }
        if (!loadedAsWmo && modelId == 0) {
            auto wmoData = assetManager_->readFile(modelPath);
            if (!wmoData.empty()) {
                pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
                LOG_DEBUG("Gameobject WMO root loaded: ", modelPath, " nGroups=", wmoModel.nGroups);
                int loadedGroups = 0;
                if (wmoModel.nGroups > 0) {
                    for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                        bool loaded = false;
                        for (const std::string& groupPath :
                             pipeline::wmoGroupCandidates(modelPath, gi)) {
                            std::vector<uint8_t> groupData =
                                assetManager_->readFile(groupPath);
                            if (groupData.empty()) continue;
                            pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                            loadedGroups++;
                            loaded = true;
                            break;
                        }
                        if (!loaded) {
                            LOG_WARNING("  Failed to load WMO group ", gi, " for: ", modelPath);
                        }
                    }
                }

                if (loadedGroups > 0 || wmoModel.nGroups == 0) {
                    modelId = assets_.nextGameObjectWmoModelId_++;
                    if (wmoRenderer->loadModel(wmoModel, modelId)) {
                        assets_.gameObjectDisplayIdWmoCache_[displayId] = modelId;
                        loadedAsWmo = true;
                    } else {
                        LOG_WARNING("Failed to load gameobject WMO model: ", modelPath);
                    }
                } else {
                    LOG_WARNING("No WMO groups loaded for gameobject: ", modelPath,
                                " - falling back to M2");
                }
            } else {
                LOG_WARNING("Failed to read gameobject WMO: ", modelPath, " - falling back to M2");
            }
        }

        if (loadedAsWmo) {
            uint32_t instanceId = scene().worldModels().createInstance(modelId, renderPos,
                glm::vec3(0.0f, 0.0f, renderYawWmo), scale);
            if (instanceId == 0) {
                LOG_WARNING("Failed to create gameobject WMO instance for guid 0x", std::hex, guid, std::dec);
                return;
            }

            spawns_->gameObjectInstances_[guid] = {.modelId = modelId, .instanceId = instanceId, .isWmo = true};
            LOG_DEBUG("Spawned gameobject WMO: guid=0x", std::hex, guid, std::dec,
                     " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");

            // Spawn transport WMO doodads (chairs, furniture, etc.) as child M2 instances
            bool isTransport = false;
            if (gameHandler_) {
                std::string lowerModelPath = modelPath;
                std::transform(lowerModelPath.begin(), lowerModelPath.end(), lowerModelPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                isTransport = (lowerModelPath.find("transport") != std::string::npos);
            }

            auto* m2Renderer = renderer_->getM2Renderer();
            if (m2Renderer && isTransport) {
                const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(modelId);
                if (doodadTemplates && !doodadTemplates->empty()) {
                    constexpr size_t kMaxTransportDoodads = 192;
                    const size_t doodadBudget = std::min(doodadTemplates->size(), kMaxTransportDoodads);
                    LOG_DEBUG("Queueing ", doodadBudget, "/", doodadTemplates->size(),
                             " transport doodads for WMO instance ", instanceId);
                    spawns_->pendingTransportDoodadBatches_.push_back(PendingTransportDoodadBatch{
                        guid,
                        modelId,
                        instanceId,
                        0,
                        doodadBudget,
                        0,
                        x, y, z,
                        orientation
                    });
                } else {
                LOG_DEBUG("Transport WMO has no doodads or templates not available");
            }
            }

            // Transport GameObjects are not always named "transport" in their WMO path
            // (e.g. elevators/lifts). If the server marks it as a transport, always
            // notify so TransportManager can animate/carry passengers.
            bool isTG = gameHandler_ && gameHandler_->isTransportGuid(guid);
            LOG_DEBUG("WMO GO spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " isTransport=", isTG,
                       " pos=(", x, ", ", y, ", ", z, ")");
            if (isTG) {
                gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }

            return;
        }

        // WMO failed - fall through to try as M2
        // Convert .wmo path to .m2 for fallback
        modelPath = modelPath.substr(0, modelPath.size() - 4) + ".m2";
    }

    {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (!m2Renderer) return;

        // Skip displayIds that permanently failed to load (e.g. empty/unsupported M2s).
        // Without this guard the same empty model is re-parsed every frame, causing
        // sustained log spam and wasted CPU.
        if (assets_.gameObjectDisplayIdFailedCache_.count(displayId)) return;

        uint32_t modelId = 0;
        auto itCache = assets_.gameObjectDisplayIdModelCache_.find(displayId);
        if (itCache != assets_.gameObjectDisplayIdModelCache_.end()) {
            modelId = itCache->second;
            if (!m2Renderer->hasModel(modelId)) {
                LOG_WARNING("GO M2 cache hit but model gone: displayId=", displayId,
                            " modelId=", modelId, " path=", modelPath,
                            " - reloading");
                assets_.gameObjectDisplayIdModelCache_.erase(itCache);
                itCache = assets_.gameObjectDisplayIdModelCache_.end();
            }
        }
        if (itCache == assets_.gameObjectDisplayIdModelCache_.end()) {
            modelId = assets_.nextGameObjectModelId_++;

            auto m2Data = assetManager_->readFile(modelPath);
            if (m2Data.empty()) {
                LOG_WARNING("Failed to read gameobject M2: ", modelPath);
                assets_.gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
            // Collision classification needs the asset path. Embedded M2 names
            // are often generic and caused herb/grass gameobjects to be treated
            // as solid props.
            model.name = modelPath;
            if (model.vertices.empty()) {
                LOG_WARNING("Failed to parse gameobject M2: ", modelPath);
                assets_.gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty() && model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, model);
            } else if (skinData.empty() && model.version >= 264) {
                LOG_WARNING("GO skin file MISSING for WotLK M2 (no indices/batches): ", skinPath);
            }

            LOG_DEBUG("GO model: ", modelPath, " v=", model.version,
                     " verts=", model.vertices.size(),
                     " idx=", model.indices.size(),
                     " batches=", model.batches.size(),
                     " bones=", model.bones.size(),
                     " skin=", (skinData.empty() ? "MISSING" : "ok"));

            if (!m2Renderer->loadModel(model, modelId)) {
                LOG_WARNING("Failed to load gameobject model: ", modelPath);
                assets_.gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            // Keep game object models resident across the away-and-back cycle.
            // Leaving town drops every instance of them, and the 60s reaper then
            // evicted the model - the log showed PostBoxHuman.m2 (the mailbox)
            // going through exactly that reap/reload churn on every return trip.
            m2Renderer->setModelPinned(modelId, true);
            assets_.gameObjectDisplayIdModelCache_[displayId] = modelId;
        }

        uint32_t instanceId = scene().models().createInstance(modelId, renderPos,
            glm::vec3(0.0f, 0.0f, renderYawM2go), scale);
        if (instanceId == 0) {
            LOG_WARNING("Failed to create gameobject instance for guid 0x", std::hex, guid, std::dec);
            return;
        }

        // Server game objects are gameplay props, not scenery: exempt them from the
        // adaptive doodad render distance, which drops to ~200 units in a city and
        // was hiding mailboxes/chests well inside the range the server still
        // considers them visible.
        scene().models().setInstanceIsGameObject(instanceId, true);

        // Deeprun Tram cars: riding never used real mesh collision to begin with (Z is
        // fully code-locked to the transport's simulated position while boarded, not
        // derived from a floor query), so the solid SubwayCar.m2 body was only ever in
        // the way - reported live as getting physically stuck walking back across a car
        // after crossing it once. Skip collision so the model is purely visual/decorative
        // for movement purposes, matching how the boarding logic already treats it (a
        // proximity/footprint check, not a physical block).
        if (displayId == 3831u) {
            scene().models().setSkipCollision(instanceId, true);
        }

        spawns_->gameObjectInstances_[guid] = {.modelId = modelId, .instanceId = instanceId, .isWmo = false};

        // Its animation is its state's: the one the server described before
        // this model existed.
        if (auto pending = spawns_->gameObjectPendingState_.find(guid);
            pending != spawns_->gameObjectPendingState_.end()) {
            applyGameObjectState(guid, pending->second);
        }

        // Notify transport system for M2 transports (e.g. Deeprun Tram cars)
        if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
            LOG_DEBUG("M2 transport spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " instanceId=", instanceId);
            gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
        }
    }

    LOG_DEBUG("Spawned gameobject: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

} // namespace core
} // namespace wowee
