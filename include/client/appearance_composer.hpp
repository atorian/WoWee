#pragma once

#include "pipeline/geoset_rules.hpp"
#include "game/character.hpp"
#include "game/inventory.hpp"
#include "rendering/animation/weapon_type.hpp"
#include <glm/glm.hpp>
#include <future>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace wowee {

namespace rendering { class Renderer; }
namespace pipeline { class AssetManager; class DBCLayout; class GameTables; struct M2Model; }
namespace game { class GameHandler; class Inventory; }

namespace core {

class PlayerView;

class EntitySpawner;

/// Resolved texture paths from CharSections.dbc for player character compositing.
struct PlayerTextureInfo {
    std::string bodySkinPath;
    std::string faceLowerPath;
    std::string faceUpperPath;
    std::string hairTexturePath;
    /// CharSections' second texture on the skin row - the "Extra" art an HD
    /// character model asks for as texture type 8. Empty on the stock models,
    /// which have no type-8 texture and never look for one.
    std::string skinExtraPath;
    std::vector<std::string> underwearPaths;
};

/// Handles player character visual appearance: skin compositing, geoset selection,
/// texture path lookups, and equipment weapon rendering.
class AppearanceComposer {
public:
    AppearanceComposer(rendering::Renderer* renderer,
                       PlayerView* playerView,
                       pipeline::AssetManager* assetManager,
                       const pipeline::GameTables& tables,
                       game::GameHandler* gameHandler,
                       EntitySpawner* entitySpawner);

    // Player model path resolution

    /// The character's skin, face, hair and skin-extra art from CharSections.dbc,
    /// bound to the model's runtime slots by compositePlayerSkin.
    /// `useFemaleModel` is the body a nonbinary character chose. The skin
    /// textures are picked per sex, so a female body reading male skins is
    /// the same mismatch the model path had.
    PlayerTextureInfo resolvePlayerTextures(const pipeline::M2Model& model,
                                            game::Race race, game::Gender gender,
                                            uint32_t appearanceBytes,
                                            bool useFemaleModel = false);

    // Apply composited textures to loaded model instance.
    // Call AFTER charRenderer->loadModel(). Saves skin state for re-compositing.
    void compositePlayerSkin(uint32_t modelSlotId, const PlayerTextureInfo& texInfo);

    // Build default active geosets for player character
    std::unordered_set<uint16_t> buildDefaultPlayerGeosets(uint8_t raceId, uint8_t sexId,
                                                           uint8_t hairStyleId, uint8_t facialId);

    /// Hang what the character wears on it again - helm, shoulders, weapons,
    /// their enchants - from the inventory and the sheath state. Its models
    /// and textures are made ready on a worker and put on in update(), a frame
    /// or two later; until then the character keeps what it has. They were
    /// read and parsed here, on the frame, at world entry and on every
    /// sheathe.
    void loadEquippedWeapons();
    /// Puts the equipment on once it is ready. Every frame.
    void update();

    // Weapon sheathe state
    void setWeaponsSheathed(bool sheathed) { weaponsSheathed_ = sheathed; }
    [[nodiscard]] bool isWeaponsSheathed() const { return weaponsSheathed_; }
    void toggleWeaponsSheathed() { weaponsSheathed_ = !weaponsSheathed_; }
    /// Where the item in a hand is put away; NONE when the hand is empty.
    [[nodiscard]] rendering::SheathSpot sheathSpot(game::EquipSlot slot) const;

    // Ranged weapon swap: temporarily show ranged weapon in right hand
    void showRangedWeapon(bool show);
    [[nodiscard]] bool isShowingRanged() const { return showingRanged_; }

    // Mining casts temporarily replace the held main-hand model with a pickaxe.
    void showMiningPick(bool show);

    // Fishing casts temporarily replace the held main-hand model with a pole
    // found in the player's equipped slots or bags.
    void showFishingPole(bool show);

    // Saved skin state accessors (used by game_screen.cpp for equipment re-compositing)
    [[nodiscard]] const std::string& getBodySkinPath() const { return bodySkinPath_; }
    [[nodiscard]] const std::vector<std::string>& getUnderwearPaths() const { return underwearPaths_; }
    [[nodiscard]] uint32_t getSkinTextureSlotIndex() const { return skinTextureSlotIndex_; }
    [[nodiscard]] uint32_t getCloakTextureSlotIndex() const { return cloakTextureSlotIndex_; }

    ~AppearanceComposer();
    AppearanceComposer(const AppearanceComposer&) = delete;
    AppearanceComposer& operator=(const AppearanceComposer&) = delete;

private:
    /// One model the character wears on a bone, as the inventory says.
    struct WornAttachment {
        const char* what;               ///< "helm", "shoulder", "weapon", "ranged weapon"
        uint32_t attachmentId = 0;
        std::string modelPath;          ///< the file on disk, chosen among the candidates
        std::string texturePath;
        glm::mat4 localTransform{1.0f};
        uint32_t enchantId = 0;         ///< shown on it, 0 for none
    };
    /// Everything worn, in the order it is hung: helm, shoulders, the hands,
    /// the ranged weapon when the main hand is empty.
    std::vector<WornAttachment> wornAttachments() const;
    struct PreparedEquipment;
    void startPreparingEquipment();
    void applyEquipment(PreparedEquipment& prepared);

    /// From the shared unit model cache; while equipment is put on, from what
    /// was prepared for it.
    bool loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel);

    rendering::Renderer* renderer_;

    PlayerView* playerView_;
    pipeline::AssetManager* assetManager_;
    const pipeline::GameTables& tables_;
    game::GameHandler* gameHandler_;
    EntitySpawner* entitySpawner_;

    // Saved at spawn for skin re-compositing on equipment changes
    std::string bodySkinPath_;
    std::vector<std::string> underwearPaths_;
    uint32_t skinTextureSlotIndex_ = 0;
    uint32_t cloakTextureSlotIndex_ = 0;

    bool weaponsSheathed_ = false;
    bool showingRanged_ = false;
    bool showingMiningPick_ = false;
    uint32_t miningPickInstanceId_ = 0;
    bool showingFishingPole_ = false;

    /// The equipment being made ready, and whether it was asked for again
    /// meanwhile (and so is out of date when it arrives).
    std::future<std::shared_ptr<PreparedEquipment>> preparing_;
    bool equipmentAskedAgain_ = false;
    /// What applyEquipment is putting on, for loadWeaponM2.
    const PreparedEquipment* applying_ = nullptr;
};

} // namespace core
} // namespace wowee
