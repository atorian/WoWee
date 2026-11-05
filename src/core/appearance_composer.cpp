#include "core/appearance_composer.hpp"
#include "core/player_view.hpp"
#include "core/geoset_rules.hpp"
#include "pipeline/item_textures.hpp"
#include "core/character_paths.hpp"
#include "core/helm_visual.hpp"
#include "core/enchant_visual.hpp"
#include "core/weapon_sheath.hpp"
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/animation_controller.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/required_table.hpp"
#include "game/game_handler.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/m2_model_cache.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <unordered_map>

namespace wowee {
namespace core {

AppearanceComposer::AppearanceComposer(rendering::Renderer* renderer,
                                       PlayerView* playerView,
                                       pipeline::AssetManager* assetManager,
                                       const pipeline::GameTables& tables,
                                       game::GameHandler* gameHandler,
                                       EntitySpawner* entitySpawner)
    : renderer_(renderer)
    , playerView_(playerView)
    , assetManager_(assetManager)
    , tables_(tables)
    , gameHandler_(gameHandler)
    , entitySpawner_(entitySpawner)
{
}

PlayerTextureInfo AppearanceComposer::resolvePlayerTextures(const pipeline::M2Model& model,
                                                            game::Race race, game::Gender gender,
                                                            uint32_t appearanceBytes,
                                                            bool useFemaleModel) {
    PlayerTextureInfo result;

    uint32_t targetRaceId = static_cast<uint32_t>(race);
    // The same rule the model path uses: a nonbinary character wears the body
    // they chose, and the skins have to be that body's.
    const bool female = (gender == game::Gender::FEMALE) ||
                        (gender == game::Gender::NONBINARY && useFemaleModel);
    uint32_t targetSexId = female ? 1u : 0u;

    result.bodySkinPath = defaultBodySkinPath(targetRaceId, targetSexId);

    const AppearanceBytes look = unpackAppearanceBytes(appearanceBytes);
    const uint8_t charSkinId = look.skinId;
    const uint8_t charFaceId = look.faceId;
    const uint8_t charHairStyleId = look.hairStyleId;
    const uint8_t charHairColorId = look.hairColorId;
    LOG_INFO("Appearance: skin=", static_cast<int>(charSkinId), " face=", static_cast<int>(charFaceId),
             " hairStyle=", static_cast<int>(charHairStyleId), " hairColor=", static_cast<int>(charHairColorId));

    // CharSections, through the one reader in pipeline/char_sections.hpp.
    //
    // This scan used to be written out here, and again in entity_spawner for
    // NPCs, and again in character_preview for the portrait - three readings of
    // one table that did not agree on what they read. Only this one looked at
    // the skin row's second texture, so ears and eyelashes were unbound on the
    // other two; only this one had a fallback for a face the table does not
    // carry. Every fault in this area had to be fixed two or three times.
    {
        const pipeline::RequiredTable charSectionsDbc(tables_, "CharSections.dbc",
                                                      "the player's skin, face and hair");
        const auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), &charSectionsDbc.layout());

        pipeline::CharacterAppearance who;
        who.raceId = targetRaceId;
        who.sexId = targetSexId;
        who.skinId = charSkinId;
        who.faceId = charFaceId;
        who.hairStyleId = charHairStyleId;
        who.hairColorId = charHairColorId;

        // The underwear rows name art that is not always on disk, and this
        // caller can ask.
        const auto sections = pipeline::resolveCharacterSections(
            charSectionsDbc.get(), csF, who,
            [](const std::string& path, void* ctx) {
                return static_cast<pipeline::AssetManager*>(ctx)->fileExists(path);
            },
            assetManager_);

        if (!sections.bodySkin.empty()) result.bodySkinPath = sections.bodySkin;
        result.skinExtraPath = sections.skinExtra;
        result.faceLowerPath = sections.faceLower;
        result.faceUpperPath = sections.faceUpper;
        if (!sections.hair.empty()) result.hairTexturePath = sections.hair;
        result.underwearPaths = sections.underwear;

        if (!sections.exactFace) {
            LOG_WARNING("No DBC face match for face=", static_cast<int>(charFaceId),
                        " skin=", static_cast<int>(charSkinId),
                        " race=", targetRaceId, " sex=", targetSexId,
                        sections.haveFace ? " - using the nearest face instead"
                                          : " - this character will render with no face");
        }
        if (!sections.haveHair) {
            LOG_WARNING("No DBC hair match for style=", static_cast<int>(charHairStyleId),
                        " color=", static_cast<int>(charHairColorId),
                        " race=", targetRaceId, " sex=", targetSexId);
        }
    }

    // Everything the head detail depends on, in one line, whichever way it went.
    // Skin-coloured eyelashes are what you see when type 8 falls back to the
    // body or pelvis art, and the three things that decide it - whether the
    // model asks for type 8, whether CharSections offered an extra texture, and
    // what is bound (compositePlayerSkin) - cannot be told apart from a
    // screenshot.
    {
        bool modelWantsExtra = false;
        for (const auto& tex : model.textures) modelWantsExtra = modelWantsExtra || tex.type == 8;
        const std::string bound = modelWantsExtra
            ? pipeline::skinExtraArt(result.skinExtraPath, result.underwearPaths, result.bodySkinPath)
            : std::string();
        LOG_WARNING("Character head detail: model asks for type 8: ",
                    (modelWantsExtra ? "yes" : "no"),
                    " | CharSections extra: '", result.skinExtraPath,
                    "' | bound: '", bound,
                    "' | body: '", result.bodySkinPath, "'");
    }

    return result;
}

void AppearanceComposer::compositePlayerSkin(uint32_t modelSlotId, const PlayerTextureInfo& texInfo) {
    if (!renderer_) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;

    // Save skin composite state for re-compositing on equipment changes
    // Include face textures so compositeWithRegions can rebuild the full base
    bodySkinPath_ = texInfo.bodySkinPath;
    underwearPaths_.clear();
    if (!texInfo.faceLowerPath.empty()) underwearPaths_.push_back(texInfo.faceLowerPath);
    if (!texInfo.faceUpperPath.empty()) underwearPaths_.push_back(texInfo.faceUpperPath);
    for (const auto& up : texInfo.underwearPaths) underwearPaths_.push_back(up);

    // Composite body skin + face + underwear overlays
    {
        std::vector<std::string> layers;
        layers.push_back(texInfo.bodySkinPath);
        if (!texInfo.faceLowerPath.empty()) layers.push_back(texInfo.faceLowerPath);
        if (!texInfo.faceUpperPath.empty()) layers.push_back(texInfo.faceUpperPath);
        for (const auto& up : texInfo.underwearPaths) {
            layers.push_back(up);
        }
        if (layers.size() > 1) {
            rendering::TextureRef compositeTex = charRenderer->compositeTextures(layers);
            if (compositeTex) {
                // Find type-1 (skin) texture slot and replace with composite
                // We need model texture info - walk slots via charRenderer
                // Use the model slot ID to find the right texture index
                auto* modelData = charRenderer->getModelData(modelSlotId);
                if (modelData) {
                    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                        if (modelData->textures[ti].type == 1) {
                            charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), compositeTex);
                            skinTextureSlotIndex_ = static_cast<uint32_t>(ti);
                            LOG_INFO("Replaced type-1 texture slot ", ti, " with composited body+face+underwear");
                            break;
                        }
                    }
                }
            }
        }
    }

    // Override hair texture on GPU (type-6 slot) after model load
    if (!texInfo.hairTexturePath.empty()) {
        rendering::TextureRef hairTex = charRenderer->loadTexture(texInfo.hairTexturePath);
        if (hairTex) {
            auto* modelData = charRenderer->getModelData(modelSlotId);
            if (modelData) {
                for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                    if (modelData->textures[ti].type == 6) {
                        charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), hairTex);
                        LOG_INFO("Applied DBC hair texture to slot ", ti, ": ", texInfo.hairTexturePath);
                        break;
                    }
                }
            }
        }
    }

    // The skin extra (type 8): ears, horns, the head-detail sheet. Its name
    // used to be written into the model, which the renderer never reads, and
    // the slot drew nothing.
    if (const std::string& extraPath = pipeline::skinExtraArt(texInfo.skinExtraPath, texInfo.underwearPaths,
                                                              texInfo.bodySkinPath);
        !extraPath.empty()) {
        rendering::TextureRef extraTex = charRenderer->loadTexture(extraPath);
        const auto* modelData = charRenderer->getModelData(modelSlotId);
        if (extraTex && extraTex != charRenderer->whiteTexture() && modelData) {
            for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                if (modelData->textures[ti].type == 8)
                    charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), extraTex);
            }
        }
    }

    // Find cloak (type-2, Object Skin) texture slot index
    {
        auto* modelData = charRenderer->getModelData(modelSlotId);
        if (modelData) {
            for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                if (modelData->textures[ti].type == 2) {
                    cloakTextureSlotIndex_ = static_cast<uint32_t>(ti);
                    LOG_INFO("Cloak texture slot: ", ti);
                    break;
                }
            }
        }
    }
}

std::unordered_set<uint16_t> AppearanceComposer::buildDefaultPlayerGeosets(uint8_t raceId, uint8_t sexId,
                                                                           uint8_t hairStyleId, uint8_t facialId) {
    // Look up the hair scalp and the facial features this character wears, then
    // ask for the bare set around them. Which geosets a character shows with
    // nothing equipped is one answer, in core/geoset_rules.hpp, shared with the
    // portrait - the two used to keep their own and had drifted.
    uint16_t selectedHairScalp = 1;
    uint16_t facial100 = 0, facial200 = 0, facial300 = 0;
    bool haveFacial = false;
    if (entitySpawner_) {
        const auto& hairMap = entitySpawner_->getHairGeosetMap();
        auto itHair = hairMap.find(appearanceKey(raceId, sexId, hairStyleId));
        if (itHair != hairMap.end() && itHair->second > 0) selectedHairScalp = itHair->second;

        const auto& facialMap = entitySpawner_->getFacialHairGeosetMap();
        auto itFacial = facialMap.find(appearanceKey(raceId, sexId, facialId));
        if (itFacial != facialMap.end()) {
            facial100 = itFacial->second.geoset100;
            facial200 = itFacial->second.geoset200;
            facial300 = itFacial->second.geoset300;
            haveFacial = true;
        }
    }
    if (!haveFacial) {
        // No row for this character: the "none" variant of all three channels.
        facial100 = facial200 = facial300 = 1;
    }

    std::unordered_set<uint16_t> activeGeosets =
        bareCharacterGeosets(selectedHairScalp, facial100, facial200, facial300, raceId);

    return activeGeosets;
}

/// The equipment's models and decoded textures, made ready on a worker.
struct AppearanceComposer::PreparedEquipment {
    std::vector<WornAttachment> worn;
    /// By lower-case, backslashed path; null where a file did not load.
    std::unordered_map<std::string, std::shared_ptr<const pipeline::M2Model>> models;
    std::unordered_map<std::string, pipeline::BLPImage> textures;
};

namespace {

/// How the unit model cache and the character renderer key a file.
std::string assetKey(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

}  // namespace

AppearanceComposer::~AppearanceComposer() {
    // Its worker reads the shared caches, which outlive this: it finishes first.
    if (preparing_.valid()) preparing_.wait();
}

bool AppearanceComposer::loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    std::shared_ptr<const pipeline::M2Model> model;
    if (applying_) {
        const auto ready = applying_->models.find(assetKey(m2Path));
        if (ready == applying_->models.end()) {
            LOG_ERROR("Equipment model ", m2Path, " was not prepared with the rest of the equipment; not shown");
            return false;
        }
        model = ready->second;
    } else {
        // The mining pick, the fishing pole, the ranged swap: parsed once by
        // the shared cache, on this thread the first time each is shown.
        model = renderer_->unitModels().load(m2Path);
    }
    if (!model) return false;
    outModel = *model;
    return true;
}

std::vector<AppearanceComposer::WornAttachment> AppearanceComposer::wornAttachments() const {
    std::vector<WornAttachment> worn;
    const auto& inventory = gameHandler_->inventory().items();
    uint8_t raceId = 0;
    uint8_t genderId = 0;
    if (const auto* ch = gameHandler_->getActiveCharacter()) {
        raceId = static_cast<uint8_t>(ch->race);
        genderId = static_cast<uint8_t>(ch->gender);
    }
    const auto displayOf = [this](const game::ItemSlot& slot) -> uint32_t {
        const auto* info = gameHandler_->inventory().getItemInfo(slot.item.itemId);
        return info && info->valid ? info->displayInfoId : slot.item.displayInfoId;
    };
    // The race's own model where it has one, else the shared one.
    const auto onDisk = [this](const HelmVisual& visual) {
        return !visual.racialModelPath.empty() && assetManager_->fileExists(visual.racialModelPath)
                   ? visual.racialModelPath
                   : visual.baseModelPath;
    };

    // Head gear, which the local character once did not have at all. Hiding
    // the helm is a display choice, not an unequip: the item stays on, the
    // model comes off, and the hair comes back.
    if (const auto& head = inventory.getEquipSlot(game::EquipSlot::HEAD);
        gameHandler_->player().helmVisible() && !head.empty()) {
        if (const uint32_t displayId = displayOf(head)) {
            const HelmVisual helm = resolveHelmVisual(*assetManager_, tables_, displayId, raceId, genderId);
            if (helm.valid())
                worn.push_back({.what = "helm", .attachmentId = kAttachHelm, .modelPath = onDisk(helm),
                                .texturePath = helm.texturePath});
        }
    }

    // The shoulders' two models, found missing after the helm.
    if (const auto& shoulders = inventory.getEquipSlot(game::EquipSlot::SHOULDERS); !shoulders.empty()) {
        if (const uint32_t displayId = displayOf(shoulders)) {
            const ShoulderVisual pair = resolveShoulderVisual(*assetManager_, tables_, displayId, raceId, genderId);
            for (const auto& [side, attachmentId] : {std::pair{&pair.left, kAttachShoulderLeft},
                                                     std::pair{&pair.right, kAttachShoulderRight}}) {
                if (side->valid())
                    worn.push_back({.what = "shoulder", .attachmentId = attachmentId, .modelPath = onDisk(*side),
                                    .texturePath = side->texturePath});
            }
        }
    }

    const pipeline::RequiredTable displayInfoDbc(tables_, "ItemDisplayInfo.dbc", "the player's weapons");
    // A display's model and texture, from whichever of the Weapon and Shield
    // folders has it.
    const auto weaponArt = [&](uint32_t displayInfoId, WornAttachment& weapon) {
        const int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) {
            LOG_WARNING("loadEquippedWeapons: displayInfoId ", displayInfoId, " not found in DBC");
            return false;
        }
        // The left pair first, the right one when there is none.
        const auto art = pipeline::readItemDisplayArt(displayInfoDbc, static_cast<uint32_t>(recIdx));
        if (art.modelFile.empty()) {
            LOG_WARNING("loadEquippedWeapons: empty model name for displayInfoId ", displayInfoId);
            return false;
        }
        weapon.modelPath = "Item\\ObjectComponents\\Weapon\\" + art.modelFile;
        if (!assetManager_->fileExists(weapon.modelPath))
            weapon.modelPath = "Item\\ObjectComponents\\Shield\\" + art.modelFile;
        if (!art.textureName.empty()) {
            weapon.texturePath = "Item\\ObjectComponents\\Weapon\\" + art.textureName + ".blp";
            if (!assetManager_->fileExists(weapon.texturePath))
                weapon.texturePath = "Item\\ObjectComponents\\Shield\\" + art.textureName + ".blp";
        }
        return true;
    };

    // The hands, held or put away by the sheath state and the item's type.
    bool mainHandWorn = false;
    for (const auto& [slot, isMainHand] : {std::pair{game::EquipSlot::MAIN_HAND, true},
                                           std::pair{game::EquipSlot::OFF_HAND, false}}) {
        const auto& equipped = inventory.getEquipSlot(slot);
        if (equipped.empty() || equipped.item.displayInfoId == 0) continue;
        const uint32_t attachmentId = weaponAttachment(weaponsSheathed_, slot, equipped.item.inventoryType);
        if (attachmentId == UINT32_MAX) continue;
        WornAttachment weapon{.what = "weapon", .attachmentId = attachmentId};
        if (!weaponArt(equipped.item.displayInfoId, weapon)) continue;
        weapon.localTransform = weaponLocalTransform(weaponsSheathed_, equipped.item.inventoryType);
        const uint64_t itemGuid = gameHandler_->inventory().getEquipSlotGuid(static_cast<int>(slot));
        weapon.enchantId = itemGuid != 0 ? gameHandler_->inventory().shownEnchantId(itemGuid) : 0;
        mainHandWorn = mainHandWorn || isMainHand;
        worn.push_back(std::move(weapon));
    }

    // A bow, gun or thrown weapon in the right hand when it holds nothing else.
    const auto& ranged = inventory.getEquipSlot(game::EquipSlot::RANGED);
    if (!mainHandWorn && !ranged.empty() && ranged.item.displayInfoId != 0) {
        WornAttachment weapon{.what = "ranged weapon",
                              .attachmentId = weaponsSheathed_ ? kAttachBack : kAttachRightHand};
        if (weaponArt(ranged.item.displayInfoId, weapon)) {
            weapon.localTransform = weaponsSheathed_ ? weaponLocalTransform(true, game::InvType::TWO_HAND)
                                                     : glm::mat4(1.0f);
            worn.push_back(std::move(weapon));
        }
    }
    return worn;
}

void AppearanceComposer::loadEquippedWeapons() {
    // One at a time: asked again while one is being made ready, that one is
    // out of date when it comes, and another is made.
    if (preparing_.valid()) {
        equipmentAskedAgain_ = true;
        return;
    }
    startPreparingEquipment();
}

void AppearanceComposer::startPreparingEquipment() {
    if (!renderer_ || !assetManager_ || !assetManager_->isInitialized() || !gameHandler_) return;
    auto prepared = std::make_shared<PreparedEquipment>();
    prepared->worn = wornAttachments();
    std::vector<std::string> modelPaths;
    std::vector<std::string> texturePaths;
    for (const auto& worn : prepared->worn) {
        modelPaths.push_back(worn.modelPath);
        texturePaths.push_back(worn.texturePath);
        if (worn.enchantId != 0)
            for (const auto& path : resolveEnchantVisualModels(tables_, worn.enchantId)) modelPaths.push_back(path);
    }
    preparing_ = ThreadPool::ioWorkers().submit(
        "player equipment", [prepared, modelPaths = std::move(modelPaths), texturePaths = std::move(texturePaths),
                             assets = assetManager_, models = &renderer_->unitModels()]() mutable {
            for (const auto& path : modelPaths) {
                if (path.empty()) continue;
                auto model = models->load(path);
                if (model)
                    for (const auto& texture : model->textures)
                        if (texture.type == 0) texturePaths.push_back(texture.filename);
                prepared->models[assetKey(path)] = std::move(model);
            }
            for (const auto& path : texturePaths) {
                if (path.empty()) continue;
                auto key = assetKey(path);
                if (!prepared->textures.count(key))
                    prepared->textures[std::move(key)] = rendering::CharacterRenderer::prepareTexture(assets, path);
            }
            return prepared;
        });
}

void AppearanceComposer::update() {
    if (!preparing_.valid() || preparing_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto prepared = preparing_.get();
    if (equipmentAskedAgain_) {
        equipmentAskedAgain_ = false;
        startPreparingEquipment();
        return;
    }
    applyEquipment(*prepared);
}

void AppearanceComposer::applyEquipment(PreparedEquipment& prepared) {
    // Equipment refreshes can arrive during a gather cast. Keep the temporary
    // tool authoritative until the cast-end callback restores real equipment.
    const uint32_t currentInstanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    if (showingMiningPick_ && currentInstanceId == miningPickInstanceId_) return;
    if (showingMiningPick_) {
        showingMiningPick_ = false;
        miningPickInstanceId_ = 0;
    }
    showingRanged_ = false;
    if (renderer_ && playerView_->getAnimationController())
        playerView_->getAnimationController()->setRangedWeaponActive(false);
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || !entitySpawner_) return;
    const uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    // Everything below from what the worker made ready, and to the GPU in one
    // batch.
    applying_ = &prepared;
    charRenderer->setPredecodedBLPCache(&prepared.textures);
    auto* uploadCtx = renderer_->getVkContext();
    if (uploadCtx) uploadCtx->beginUploadBatch();
    struct Guard {
        AppearanceComposer* composer;
        rendering::CharacterRenderer* renderer;
        rendering::VkContext* ctx;
        ~Guard() {
            composer->applying_ = nullptr;
            renderer->setPredecodedBLPCache(nullptr);
            if (ctx) ctx->endUploadBatch();
        }
    } guard{this, charRenderer, uploadCtx};

    // Equipment reloads and Z toggles can move models between these points.
    // Clear every one, so old copies never remain behind. Only the helm's
    // point for the head: detaching 0 as well would drop the shield.
    auto& characters = renderer_->scene().characters();
    characters.detachWeapon(charInstanceId, kAttachHelm);
    characters.detachWeapon(charInstanceId, kAttachShoulderRight);
    characters.detachWeapon(charInstanceId, kAttachShoulderLeft);
    for (uint32_t attachmentId : kWeaponAttachmentPoints) characters.detachWeapon(charInstanceId, attachmentId);

    for (const auto& worn : prepared.worn) {
        pipeline::M2Model model;
        if (!loadWeaponM2(worn.modelPath, model)) {
            LOG_WARNING("Equipped ", worn.what, " ", worn.modelPath, " did not load; not shown");
            continue;
        }
        if (!characters.attachWeapon(charInstanceId, worn.attachmentId, model,
                                     entitySpawner_->allocateWeaponModelId(), worn.texturePath,
                                     worn.localTransform))
            continue;
        LOG_INFO("Equipped ", worn.what, ": ", worn.modelPath, " at attachment ", worn.attachmentId,
                 weaponsSheathed_ ? " (sheathed)" : "");
        if (worn.enchantId != 0) {
            attachEnchantVisuals(
                characters, tables_, charInstanceId, worn.attachmentId, worn.enchantId,
                [this](const std::string& path, pipeline::M2Model& out) { return loadWeaponM2(path, out); },
                [this] { return entitySpawner_->allocateWeaponModelId(); });
        }
    }
}

rendering::SheathSpot AppearanceComposer::sheathSpot(game::EquipSlot slot) const {
    if (!gameHandler_) return rendering::SheathSpot::NONE;
    const auto& equipped = gameHandler_->inventory().items().getEquipSlot(slot);
    if (equipped.empty()) return rendering::SheathSpot::NONE;
    switch (weaponAttachment(true, slot, equipped.item.inventoryType)) {
        case kAttachHipWeaponLeft:
        case kAttachHipWeaponRight: return rendering::SheathSpot::HIP;
        case kAttachBack:
        case kAttachSheathShield:   return rendering::SheathSpot::BACK;
        default:                    return rendering::SheathSpot::NONE;
    }
}

void AppearanceComposer::showMiningPick(bool show) {
    if (show == showingMiningPick_) return;

    if (!show) {
        showingMiningPick_ = false;
        miningPickInstanceId_ = 0;
        loadEquippedWeapons();
        return;
    }

    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ ||
        !assetManager_->isInitialized() || !entitySpawner_) {
        return;
    }

    const uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    // Item 2901 (Mining Pick) resolves to ItemDisplayInfo 6568 in the WotLK DBC.
    constexpr uint32_t kMiningPickDisplayId = 6568;
    const pipeline::RequiredTable displayInfoDbc(tables_, "ItemDisplayInfo.dbc", "the mining pick");

    const int32_t recIdx = displayInfoDbc->findRecordById(kMiningPickDisplayId);
    if (recIdx < 0) {
        LOG_WARNING("showMiningPick: displayInfoId ", kMiningPickDisplayId,
                    " not found in DBC");
        return;
    }

    std::string modelName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), displayInfoDbc.column("LeftModel"));
    std::string textureName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), displayInfoDbc.column("LeftModelTexture"));
    if (modelName.empty()) return;

    const size_t dotPos = modelName.rfind('.');
    std::string modelFile = dotPos == std::string::npos
        ? modelName + ".m2" : modelName.substr(0, dotPos) + ".m2";
    const std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;

    pipeline::M2Model pickModel;
    if (!loadWeaponM2(m2Path, pickModel)) {
        LOG_WARNING("showMiningPick: failed to load ", m2Path);
        return;
    }

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
    }

    renderer_->scene().characters().detachWeapon(charInstanceId, kAttachRightHand);
    const uint32_t modelId = entitySpawner_->allocateWeaponModelId();
    if (renderer_->scene().characters().attachWeapon(charInstanceId, kAttachRightHand, pickModel,
                                   modelId, texturePath)) {
        showingMiningPick_ = true;
        miningPickInstanceId_ = charInstanceId;
        showingRanged_ = false;
        if (playerView_->getAnimationController())
            playerView_->getAnimationController()->setRangedWeaponActive(false);
        LOG_INFO("Mining pick attached at right hand: ", m2Path);
    } else {
        // Do not leave the player empty-handed if the temporary model failed.
        loadEquippedWeapons();
    }
}

void AppearanceComposer::showFishingPole(bool show) {
    if (show == showingFishingPole_) return;

    if (!show) {
        showingFishingPole_ = false;
        loadEquippedWeapons();
        return;
    }
    if (!renderer_ || !renderer_->getCharacterRenderer() || !gameHandler_ ||
        !assetManager_ || !assetManager_->isInitialized() || !entitySpawner_) {
        return;
    }

    const auto isFishingPole = [this](const game::ItemSlot& slot) {
        if (slot.empty()) return false;
        if (slot.item.subclassName == "Fishing Pole") return true;
        const auto* info = gameHandler_->inventory().getItemInfo(slot.item.itemId);
        return info && info->valid && info->itemClass == 2 && info->subClass == 20;
    };

    const game::ItemSlot* pole = nullptr;
    const auto& inventory = gameHandler_->inventory().items();
    const auto& mainHand = inventory.getEquipSlot(game::EquipSlot::MAIN_HAND);
    if (isFishingPole(mainHand)) pole = &mainHand;
    for (int i = 0; !pole && i < inventory.getBackpackSize(); ++i) {
        const auto& slot = inventory.getBackpackSlot(i);
        if (isFishingPole(slot)) pole = &slot;
    }
    for (int bag = 0; !pole && bag < game::Inventory::NUM_BAG_SLOTS; ++bag) {
        for (int slotIndex = 0; !pole && slotIndex < inventory.getBagSize(bag); ++slotIndex) {
            const auto& slot = inventory.getBagSlot(bag, slotIndex);
            if (isFishingPole(slot)) pole = &slot;
        }
    }
    if (!pole || pole->item.displayInfoId == 0) {
        LOG_WARNING("showFishingPole: no fishing pole with display data found in inventory");
        return;
    }

    const pipeline::RequiredTable displayInfoDbc(tables_, "ItemDisplayInfo.dbc", "the fishing pole");
    const int32_t recIdx = displayInfoDbc->findRecordById(pole->item.displayInfoId);
    if (recIdx < 0) return;

    std::string modelName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), displayInfoDbc.column("LeftModel"));
    std::string textureName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), displayInfoDbc.column("LeftModelTexture"));
    if (modelName.empty()) return;

    const size_t dotPos = modelName.rfind('.');
    const std::string modelFile = dotPos == std::string::npos
        ? modelName + ".m2" : modelName.substr(0, dotPos) + ".m2";
    std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
    pipeline::M2Model poleModel;
    if (!loadWeaponM2(m2Path, poleModel)) return;

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
    }

    const uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;
    renderer_->scene().characters().detachWeapon(charInstanceId, kAttachRightHand);
    const uint32_t modelId = entitySpawner_->allocateWeaponModelId();
    if (renderer_->scene().characters().attachWeapon(charInstanceId, kAttachRightHand, poleModel,
                                   modelId, texturePath)) {
        showingFishingPole_ = true;
        showingRanged_ = false;
        if (playerView_->getAnimationController())
            playerView_->getAnimationController()->setRangedWeaponActive(false);
        LOG_INFO("Fishing pole attached at right hand: ", m2Path);
    } else {
        loadEquippedWeapons();
    }
}

void AppearanceComposer::showRangedWeapon(bool show) {
    if (show == showingRanged_) return;

    if (!renderer_ || !renderer_->getCharacterRenderer() || !gameHandler_ || !assetManager_ || !assetManager_->isInitialized())
        return;

    uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    if (!show) {
        showingRanged_ = false;
        if (playerView_->getAnimationController())
            playerView_->getAnimationController()->setRangedWeaponActive(false);
        // Swap back to normal melee weapons
        loadEquippedWeapons();
        return;
    }

    auto& inventory = gameHandler_->inventory().items();
    const auto& rangedSlot = inventory.getEquipSlot(game::EquipSlot::RANGED);
    if (rangedSlot.empty() || rangedSlot.item.displayInfoId == 0) return;

    const pipeline::RequiredTable displayInfoDbc(tables_, "ItemDisplayInfo.dbc", "the player's ranged weapon");

    uint32_t displayInfoId = rangedSlot.item.displayInfoId;
    int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
    if (recIdx < 0) return;

    std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), displayInfoDbc.column("LeftModel"));
    std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), displayInfoDbc.column("LeftModelTexture"));
    if (modelName.empty()) return;

    std::string modelFile = modelName;
    {
        size_t dotPos = modelFile.rfind('.');
        if (dotPos != std::string::npos)
            modelFile = modelFile.substr(0, dotPos) + ".m2";
        else
            modelFile += ".m2";
    }

    std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
    pipeline::M2Model weaponModel;
    if (!loadWeaponM2(m2Path, weaponModel)) {
        m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
        if (!loadWeaponM2(m2Path, weaponModel)) return;
    }

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
        if (!assetManager_->fileExists(texturePath))
            texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
    }

    // Detach current right-hand weapon and attach ranged weapon
    renderer_->scene().characters().detachWeapon(charInstanceId, 1);
    uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
    bool ok = renderer_->scene().characters().attachWeapon(charInstanceId, 1, weaponModel, weaponModelId, texturePath);
    if (ok) {
        showingRanged_ = true;
        if (playerView_->getAnimationController())
            playerView_->getAnimationController()->setRangedWeaponActive(true);
        LOG_INFO("Swapped to ranged weapon: ", m2Path, " at attachment 1 (right hand)");
    }
}

} // namespace core
} // namespace wowee
