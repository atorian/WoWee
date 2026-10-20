#pragma once

#include "game/character.hpp"
#include "core/job_group.hpp"
#include "game/game_services.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/locomotion_fsm.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <chrono>
#include <optional>
#include <future>
#include <mutex>
#include <glm/glm.hpp>

namespace wowee {

// Forward declarations
namespace rendering { class Renderer; class SceneCommands; }
namespace pipeline { class AssetManager; struct M2Model; struct WMOModel; }
namespace audio { enum class VoiceType; }
namespace game { class GameHandler; }

namespace core {

class Application;
class PlayerView;

class EntitySpawner {
public:
    struct SpawnState;
    EntitySpawner(rendering::Renderer* renderer,
                  pipeline::AssetManager* assetManager,
                  game::GameHandler* gameHandler,
                  game::GameServices* gameServices,
                  PlayerView* playerView);
    ~EntitySpawner();

    EntitySpawner(const EntitySpawner&) = delete;
    EntitySpawner& operator=(const EntitySpawner&) = delete;

    // Lifecycle
    void initialize();   // Build DBC lookups
    void update();       // Process all spawn/despawn queues (called from Application::update)

    // Queue-based spawn API (called from GameHandler callbacks)
    void queueCreatureSpawn(uint64_t guid, uint32_t displayId,
                            float x, float y, float z, float orientation, float scale = 1.0f);
    void queuePlayerSpawn(uint64_t guid, uint8_t raceId, uint8_t genderId,
                          uint32_t appearanceBytes, uint8_t facialFeatures,
                          float x, float y, float z, float orientation);
    void queueGameObjectSpawn(uint64_t guid, uint32_t entry, uint32_t displayId,
                              float x, float y, float z, float orientation, float scale = 1.0f);
    /// What a player wears, per slot in the visible-item order (head, neck,
    /// shoulders ... tabard): the display to draw, its inventory type and the
    /// enchant it shows (game::events::PlayerEquipmentResolved).
    struct WornEquipment {
        std::array<uint32_t, 19> displayInfoIds{};
        std::array<uint8_t, 19> inventoryTypes{};
        std::array<uint32_t, 19> enchantIds{};
    };
    void queuePlayerEquipment(uint64_t guid, const WornEquipment& equipment);

    // Immediate despawn
    void despawnCreature(uint64_t guid);
    void despawnPlayer(uint64_t guid);
    void despawnGameObject(uint64_t guid);

    // ---- Across a reconnect --------------------------------------------------
    /// The world connection is lost and the client logs in again on this
    /// map: every creature, player and object presented now - built or still
    /// queued - is held. A despawn leaves a held entity standing; the game
    /// showing it again (shown) keeps its instance, with no vanishing and
    /// reappearing; releaseHeld takes the rest.
    void holdPresented();
    /// The game shows this entity in view. One that was held is the same one
    /// again and stays.
    void shown(uint64_t guid);
    /// The hold ends: what the server did not show again is despawned.
    /// Answers how many were.
    size_t releaseHeld();
    /// How many presented entities are held and not yet shown again.
    [[nodiscard]] size_t heldCount() const { return spawns_ ? spawns_->held_.size() : 0; }

    // Rebuild lookup tables (for reloadExpansionData after expansion change)
    void rebuildLookups();

    // Check if any spawn/async work is still pending
    bool hasWorkPending() const;

    // Status queries
    bool isCreatureSpawned(uint64_t guid) const { return spawns_ && spawns_->creatureInstances_.count(guid) > 0; }
    bool isCreaturePending(uint64_t guid) const { return spawns_ && spawns_->pendingCreatureSpawnGuids_.count(guid) > 0; }
    bool isPlayerSpawned(uint64_t guid) const { return spawns_ && spawns_->playerInstances_.count(guid) > 0; }
    bool isPlayerPending(uint64_t guid) const { return spawns_ && spawns_->pendingPlayerSpawnGuids_.count(guid) > 0; }
    bool isGameObjectSpawned(uint64_t guid) const { return spawns_ && spawns_->gameObjectInstances_.count(guid) > 0; }

    /// What the server says a game object is doing: open, closed, destroyed.
    ///
    /// A door is a pose. The state arrives twice - once in the create block for
    /// an object that is already open when it comes into view, and again
    /// whenever it changes - and the first of those used to arrive before the
    /// model existed and was dropped, so every door in the world was drawn in
    /// its closed pose however the server had it.
    void applyGameObjectState(uint64_t guid, uint8_t goState);

    // Quick instance ID lookups (returns 0 if not found)
    uint32_t getCreatureInstanceId(uint64_t guid) const {
        if (!spawns_) return 0;
        auto it = spawns_->creatureInstances_.find(guid); return (it != spawns_->creatureInstances_.end()) ? it->second : 0;
    }
    uint32_t getPlayerInstanceId(uint64_t guid) const {
        if (!spawns_) return 0;
        auto it = spawns_->playerInstances_.find(guid); return (it != spawns_->playerInstances_.end()) ? it->second : 0;
    }

    // Render bounds/position queries (used by click targeting, etc.)
    bool getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const;
    uint32_t characterInstanceIdForGuid(uint64_t guid) const;
    bool getRenderFootZForGuid(uint64_t guid, float& outFootZ) const;
    bool getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const;

    // Display data lookups
    bool areCreatureLookupsBuilt() const { return assets_.creatureLookupsBuilt_; }
    bool areGameObjectLookupsBuilt() const { return assets_.gameObjectLookupsBuilt_; }
    std::string getModelPathForDisplayId(uint32_t displayId) const;
    /// The skin textures a creature display wears, by M2 texture type - 11,
    /// 12 and 13 are skin1, skin2 and skin3 in CreatureDisplayInfo.dbc.
    ///
    /// A creature's M2 does not name these: the model declares the slots and
    /// the display row fills them, which is how one model serves a dozen
    /// recolours. Anything loading a creature model outside the spawner needs
    /// them too, or it draws an untextured shape - which is what the target
    /// frame's portrait did until this was shared.
    ///
    /// Paths are resolved against the model's own directory, and only ones the
    /// install actually has are returned.
    std::vector<std::pair<uint32_t, std::string>>
    getCreatureSkinPaths(uint32_t displayId, const std::string& modelPath) const;

    /// A humanoid creature's appearance, if this display is one.
    ///
    /// Most NPCs worth looking at are: a guard, a questgiver, an innkeeper.
    /// CreatureDisplayInfoExtra gives them a race, a sex and the same skin,
    /// face, hair and facial-hair choices a player character has - so the way
    /// to draw one is the way a character is drawn, not the way a creature is.
    /// A creature's skin fields are empty for these, which is why loading one
    /// as a creature gives an untextured shape.
    ///
    /// The appearance bytes are packed as a character's are: skin, face, hair
    /// style and hair colour, one byte each from the bottom up.
    /// The pre-composited skin for a humanoid display, or empty.
    ///
    /// This is the whole appearance already baked - skin, face, hair and the
    /// armour the NPC wears - which is how the game draws them and how this
    /// client can, without compositing anything.
    std::string getHumanoidBakePath(uint32_t displayId) const;

    bool getHumanoidAppearance(uint32_t displayId, uint8_t& race, uint8_t& sex,
                               uint32_t& appearanceBytes, uint8_t& facialHair) const;

    /// What that humanoid is wearing, as (ItemDisplayInfo id, inventory type)
    /// pairs - the shape applyEquipment reads. CreatureDisplayInfoExtra holds
    /// eleven slots in its own order, and this is that order translated into
    /// the inventory types the rest of the client speaks.
    std::vector<std::pair<uint32_t, uint8_t>>
    getHumanoidEquipment(uint32_t displayId) const;
    std::string getGameObjectModelPathForDisplayId(uint32_t displayId) const;

    /// The hull a transport is drawn with, or "" to use the display lookup.
    ///
    /// A preloaded transport can be spawned before its displayId is known, so
    /// the vehicle has to be recognisable from its GameObject entry too.
    ///
    /// One function because there were two copies of this table, in
    /// entity_spawner_player.cpp and entity_spawner_processing.cpp, and the
    /// queued path in the second is the one most spawns actually take -
    /// so fixing the first left the bug on screen.
    static std::string transportModelPath(uint32_t entry, uint32_t displayId);
    audio::VoiceType detectVoiceTypeFromDisplayId(uint32_t displayId) const;

    // Attempts one deferred attachment and owns retry bookkeeping. Returns true
    // only when this call consumed the caller's per-frame attachment budget.
    bool retryCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId,
                                     uint8_t maxAttempts);

    // Mount
    void setMountDisplayId(uint32_t displayId) { if (spawns_) spawns_->pendingMountDisplayId_ = displayId; }
    uint32_t getMountInstanceId() const { return spawns_ ? spawns_->mountInstanceId_ : 0; }
    uint32_t getMountModelId() const { return spawns_ ? spawns_->mountModelId_ : 0; }

    struct RemotePlayerMount {
        uint32_t displayId = 0;
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        float riderHeight = 0.0f;
    };
    void setRemotePlayerMountDisplayId(uint64_t guid, uint32_t displayId);
    // A rider sits at this attachment of the mount model ("MountMain", the
    // saddle), lifted a little so the seat does not show through.
    static constexpr uint32_t kMountSeatAttachment = 0;
    static constexpr float kMountSeatLift = 0.08f;
    const RemotePlayerMount* getRemotePlayerMount(uint64_t guid) const {
        if (!spawns_) return nullptr;
        auto it = spawns_->remotePlayerMounts_.find(guid);
        return it != spawns_->remotePlayerMounts_.end() ? &it->second : nullptr;
    }

    /// How another player's models are being animated: the locomotion state
    /// machine the local character uses, fed from their reported movement,
    /// and what the frame sync last started on each model. Animations other
    /// callbacks start - casts, emotes, swings - stay until the locomotion
    /// choice changes, so the sync plays only on a change.
    struct RemotePlayerLocomotion {
        static constexpr uint32_t kNothingPlayed = UINT32_MAX;
        rendering::LocomotionFSM fsm;
        rendering::AnimCapabilitySet caps;
        uint32_t characterInstance = 0;   // The model caps were probed on
        uint32_t mountInstance = 0;
        uint32_t playedCharacterAnim = kNothingPlayed;
        uint32_t playedMountAnim = kNothingPlayed;
    };
    RemotePlayerLocomotion& remotePlayerLocomotion(uint64_t guid) { return spawns_->remotePlayerLocomotion_[guid]; }


    // Local player GUID exclusion (EntitySpawner skips spawning the local player)
    void setLocalPlayerGuid(uint64_t guid) { if (spawns_) spawns_->spawnedPlayerGuid_ = guid; }

    // Weapon model IDs (shared counter for creature weapons + equipment helm/weapon models)
    uint32_t allocateWeaponModelId() { return nextWeaponModelId_++; }

    // Maximum weapon attachment operations per tick (used by Application update loop)
    static constexpr int MAX_WEAPON_ATTACHES_PER_TICK = 2;

    // Dead creature tracking (spawn in corpse/death pose)
    void markCreatureDead(uint64_t guid) { if (spawns_) spawns_->deadCreatureGuids_.insert(guid); }
    void unmarkCreatureDead(uint64_t guid) { if (spawns_) spawns_->deadCreatureGuids_.erase(guid); }
    void clearDeadCreatureGuids() { if (spawns_) spawns_->deadCreatureGuids_.clear(); }

    // Mount state management
    void clearMountState();

    // Transport registration API (used by setupUICallbacks transport spawn callback)
    void queueTransportRegistration(uint64_t guid, uint32_t entry, uint32_t displayId,
                                     float x, float y, float z, float orientation);
    void setTransportPendingMove(uint64_t guid, float x, float y, float z, float orientation);
    bool hasTransportRegistrationPending(uint64_t guid) const;
    void updateTransportRegistration(uint64_t guid, uint32_t displayId,
                                      float x, float y, float z, float orientation);

    // Test transport setup flag
    bool isTestTransportSetup() const { return spawns_ && spawns_->testTransportSetup_; }
    void setTestTransportSetup(bool v) { if (spawns_) spawns_->testTransportSetup_ = v; }

    // Creature animation state accessors (for movement sync in Application::update)
    std::unordered_map<uint64_t, uint32_t>& getCreatureInstances() { return spawns_->creatureInstances_; }
    const std::unordered_map<uint64_t, uint32_t>& getCreatureInstances() const { return spawns_->creatureInstances_; }
    std::unordered_map<uint64_t, uint32_t>& getCreatureModelIds() { return spawns_->creatureModelIds_; }
    const std::unordered_map<uint64_t, uint32_t>& getCreatureDisplayIds() const { return spawns_->creatureDisplayIds_; }
    std::unordered_map<uint64_t, glm::vec3>& getCreatureRenderPosCache() { return spawns_->creatureRenderPosCache_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasMoving() { return spawns_->creatureWasMoving_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasSwimming() { return spawns_->creatureWasSwimming_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasFlying() { return spawns_->creatureWasFlying_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasWalking() { return spawns_->creatureWasWalking_; }
    std::unordered_map<uint64_t, bool>& getCreatureSwimmingState() { return spawns_->creatureSwimmingState_; }
    std::unordered_map<uint64_t, bool>& getCreatureWalkingState() { return spawns_->creatureWalkingState_; }
    std::unordered_map<uint64_t, bool>& getCreatureFlyingState() { return spawns_->creatureFlyingState_; }
    std::unordered_map<uint64_t, uint32_t>& getCreatureActiveEmotes() { return spawns_->creatureActiveEmotes_; }
    std::unordered_map<uint32_t, bool>& getModelIdIsWolfLike() { return assets_.modelIdIsWolfLike_; }

    // Player instance accessors (for movement sync in Application::update)
    std::unordered_map<uint64_t, uint32_t>& getPlayerInstances() { return spawns_->playerInstances_; }
    const std::unordered_map<uint64_t, uint32_t>& getPlayerInstances() const { return spawns_->playerInstances_; }

    // GameObject instance accessors
    auto& getGameObjectInstances() { return spawns_->gameObjectInstances_; }
    const auto& getGameObjectInstances() const { return spawns_->gameObjectInstances_; }

    // Display data accessors (needed by Application for gryphon/wyvern display IDs)
    const auto& getDisplayDataMap() const { return assets_.displayDataMap_; }

    /// Everything needed to draw a creature somewhere other than the world:
    /// the model it uses and the skins that go on it.
    ///
    /// Both halves already live here - CreatureDisplayInfo.dbc gives the model
    /// id and the skin names, CreatureModelData.dbc turns the model id into a
    /// path - and they were reachable only from inside the spawner. A preview
    /// frame wants the same answer and reading the two files again would put a
    /// second copy of the layout somewhere else, which is how a display id ends
    /// up meaning two different creatures.
    struct CreatureModel {
        std::string m2Path;
        std::string skin1, skin2, skin3;
        uint32_t    modelId = 0;
        /// A model with no skin draws as a white shape, which is worse than an
        /// empty frame - so a caller with nowhere to get textures should treat
        /// this as nothing to draw.
        [[nodiscard]] bool valid() const { return !m2Path.empty() && !skin1.empty(); }
    };
    uint32_t getGryphonDisplayId() const { return assets_.gryphonDisplayId_; }
    uint32_t getWyvernDisplayId() const { return assets_.wyvernDisplayId_; }

    // Character section lookups (needed for player character skin compositing)
    /// The key for assets_.charSectionsCache_, in one place.
    ///
    /// It was written out twice - once where the cache is filled and once where
    /// it is read - and the two had to agree bit for bit or every lookup missed
    /// silently. Two copies of a packing rule is the same hazard as two copies
    /// of any other rule, and cheaper to remove than to remember.
    static constexpr uint64_t charSectionKey(uint8_t race, uint8_t sex, uint8_t section,
                                             uint8_t variation, uint8_t color, int texIndex) {
        return (static_cast<uint64_t>(race) << 26) |
               (static_cast<uint64_t>(sex & 0xF) << 22) |
               (static_cast<uint64_t>(section & 0xF) << 18) |
               (static_cast<uint64_t>(variation & 0xFF) << 10) |
               (static_cast<uint64_t>(color & 0xFF) << 2) |
               static_cast<uint64_t>(texIndex);
    }

    std::string lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                  uint8_t variation, uint8_t color, int texIndex = 0) const;
    const std::unordered_map<uint32_t, uint16_t>& getHairGeosetMap() const { return assets_.hairGeosetMap_; }

    struct FacialHairGeosets { uint16_t geoset100 = 0; uint16_t geoset300 = 0; uint16_t geoset200 = 0; };
    const std::unordered_map<uint32_t, FacialHairGeosets>& getFacialHairGeosetMap() const { return assets_.facialHairGeosetMap_; }

    // Creature M2 sync loader (used by spawnPlayerCharacter in Application)

private:
    /// Where the spawner's changes to the scene go (rendering/scene_commands.hpp):
    /// made at the start of the frame's Present. Only where renderer_ is set.
    rendering::SceneCommands& scene();

    // Dependencies (non-owning)
    rendering::Renderer* renderer_;
    pipeline::AssetManager* assetManager_;
    game::GameHandler* gameHandler_;
    game::GameServices* gameServices_;
    /// The player's camera and animation: a transport transfer moves the one,
    /// a mount changes the other.
    PlayerView* playerView_;

    // --- Creature display data (from DBC files) ---
    struct CreatureDisplayData {
        uint32_t modelId = 0;
        std::string skin1, skin2, skin3;  // Texture names from CreatureDisplayInfo.dbc
        uint32_t extraDisplayId = 0;      // Link to CreatureDisplayInfoExtra.dbc
        /// CreatureDisplayInfo.CreatureModelScale, which is how one model is
        /// reused at different sizes: the boar model is 0.6 for a piglet and
        /// 1.5 for a giant, over ten displays. Nothing read it, so every
        /// creature sharing a model drew at that model's own size.
        float displayScale = 1.0f;
    };

    /// CreatureModelData.ModelScale for a model id, the second half of the
    /// same product. Kept beside the display data rather than in it because it
    /// belongs to the model and several displays share one.
    /// CreatureModelData.ModelScale for the model a display names.
    ///
    /// Keyed by the display, not by a model id: the renderer's model handles
    /// are a counter of its own and share no numbering with the DBC, so
    /// asking this with one silently answered 1.0 for every creature there
    /// has ever been. 306 displays want 1.25 and two want 2.0.
    float creatureModelScale(uint32_t displayId) const;

    /// CreatureDisplayInfo.CreatureModelScale for a display id, or 1.0 where
    /// the display is unknown - an unknown display is already drawn at the
    /// model's own size, and guessing smaller would hide it.
    float creatureDisplayScale(uint32_t displayId) const;

    /// Apply the textures a creature display names: its own skin variations,
    /// and for a humanoid the composited body, face, hair and equipment. Once
    /// per display, since the model is shared by every creature using it.
    /// Start a creature in the pose the server already has it in - dead,
    /// mid-emote, or newly arrived, and fade it in.
    void playCreatureSpawnPose(uint64_t guid, uint32_t instanceId);

    /// Choose one mesh per clothing group for a character-style NPC, leaving
    /// every other batch of the model untouched. An NPC has no inventory to
    /// build a geoset set from, so its equipment comes from
    /// CreatureDisplayInfoExtra.
    void normalizeHumanoidClothingGeosets(uint32_t instanceId, uint32_t modelId,
                                          uint32_t displayId);

    /// Per-instance colouring of a humanoid NPC: hair, skin, and the
    /// head-detail sheet. Per instance rather than per model, so two NPCs
    /// sharing a model still get their own hair colour.
    void applyHumanoidInstanceOverrides(uint32_t instanceId, uint32_t modelId,
                                        uint32_t displayId);

    void applyCreatureDisplayTextures(uint32_t displayId, uint32_t modelId,
                                      const CreatureDisplayData& dispData);

    struct HumanoidDisplayExtra {
        uint8_t raceId = 0;
        uint8_t sexId = 0;
        uint8_t skinId = 0;
        uint8_t faceId = 0;
        uint8_t hairStyleId = 0;
        uint8_t hairColorId = 0;
        uint8_t facialHairId = 0;
        std::string bakeName;  // Pre-baked texture path if available
        // Equipment display IDs (from columns 8-18)
        // 0=helm, 1=shoulder, 2=shirt, 3=chest, 4=belt, 5=legs, 6=feet, 7=wrist, 8=hands, 9=tabard, 10=cape
        uint32_t equipDisplayId[11] = {0};
    };
    // CharHairGeosets.dbc: key = (raceId<<16)|(sexId<<8)|variationId → geosetId (skinSectionId)
    // CharFacialHairStyles.dbc: key = (raceId<<16)|(sexId<<8)|variationId → {geoset100, geoset300, geoset200}
    bool tryAttachCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId);

    // CharSections.dbc lookup cache
    void buildCharSectionsCache();

    // Creature display lookup building
    void buildCreatureDisplayLookups();

    // Weapon M2 loading helper
    bool loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel,
                      const std::string& texturePath = {});

    // GameObject display lookups
    void buildGameObjectDisplayLookups();

    // --- Creature instances and tracking ---
    // Drop the pending-spawn marker for guid unless a queue entry still references it.
    void erasePendingGuidIfUnqueued(uint64_t guid);

    // Latest server-requested display, including creatures whose async model load
    // has not completed yet. This prevents an older queued display from winning.
    // Server waypoint scripts hold work/chop emotes for many seconds. Locomotion
    // temporarily overrides them, then the idle transition restores this loop.

    void syncCreatureStealthVisuals();

    // --- Creature async loads ---
    struct PreparedCreatureModel {
        JobGroup::Token token;
        uint64_t guid;
        uint32_t displayId;
        uint32_t modelId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::M2Model> model;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
        bool valid = false;
        bool permanent_failure = false;
    };
    struct AsyncCreatureLoad {
        JobGroup::Future<PreparedCreatureModel> future;
        std::optional<PreparedCreatureModel> ready;
    };
    void processAsyncCreatureResults(bool unlimited = false);
    static constexpr int MAX_ASYNC_CREATURE_LOADS = 4;

    // --- NPC equipment attachment models (helm, shoulders) ---
    // Equipment is shared across NPCs, so a crowd wearing the same gear would
    // otherwise re-read, re-parse and re-upload the same M2 once per spawn. Cache
    // it by resolved path. A modelId of 0 negative-caches a path that has no model,
    // so a missing file is not retried against the disk on every spawn.
    struct CachedAttachmentModel {
        uint32_t modelId = 0;                      // 0 = no usable model
        std::shared_ptr<pipeline::M2Model> model;  // shared_ptr: M2Model is incomplete here
    };
    // Parsed geometry, keyed by path. A null value negative-caches a missing file so
    // it is not probed against the disk on every spawn.
    // Renderer model id, keyed by "path|texture". attachWeapon() binds the texture to
    // the model id, so two recolours of one mesh must not share an id or the last
    // NPC spawned would retexture every other NPC wearing that mesh.
    /// Resolve the first candidate path with a usable model, reading and parsing it
    /// at most once. Returns modelId == 0 when no candidate yields a valid model.
    CachedAttachmentModel getOrLoadAttachmentModel(
        const std::vector<std::string>& candidatePaths, const std::string& texturePath);

    /// Says which model the first few humanoid displays resolved to, so an
    /// asset overlay that re-points them can be told apart from one that never
    /// reached the client. Five is enough to see and few enough to ignore.
    /// Says what the first few NPCs resolved for their head-detail texture.

    // --- Creature spawn queue ---
    struct PendingCreatureSpawn {
        uint64_t guid;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
        JobGroup::Token token;
    };
    static constexpr int MAX_SPAWNS_PER_FRAME = 3;
    static constexpr auto CREATURE_SPAWN_RETRY_WINDOW = std::chrono::seconds(5);
    /// How many retry windows a creature has already used up.
    ///
    /// Running out used to abandon the spawn for good, and nothing ever asked
    /// again: the server does not re-send an object that is already in range,
    /// so the creature stayed missing until something made it re-send - which
    /// is what walking out of the zone and back does, and why they "appear
    /// when I zone again". A window that expires now buys another one, up to
    /// this many, so a spawn that keeps failing is delayed rather than lost.
    static constexpr int MAX_CREATURE_SPAWN_RETRY_WINDOWS = 6;
    void processCreatureSpawnQueue(bool unlimited = false);

    // --- NPC composite loads ---
    struct DeferredNpcComposite {
        uint32_t modelId;
        uint32_t displayId;
        std::string basePath;
        std::vector<std::string> overlayPaths;
        std::vector<std::pair<int, std::string>> regionLayers;
        std::vector<uint32_t> skinTextureSlots;
        bool hasComposite = false;
        bool hasSimpleSkin = false;
        std::string bakedSkinPath;
        bool hasBakedSkin = false;
        std::vector<uint32_t> hairTextureSlots;
        std::string hairTexturePath;
        bool useBakedForHair = false;
    };
    struct PreparedNpcComposite {
        JobGroup::Token token;
        DeferredNpcComposite info;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncNpcCompositeLoad {
        JobGroup::Future<PreparedNpcComposite> future;
    };
    void processAsyncNpcCompositeResults(bool unlimited = false);

    // --- Player instances ---
    struct OnlinePlayerAppearanceState {
        uint32_t instanceId = 0;
        uint32_t modelId = 0;
        uint8_t raceId = 0;
        uint8_t genderId = 0;
        uint32_t appearanceBytes = 0;
        uint8_t facialFeatures = 0;
        std::string bodySkinPath;
        std::vector<std::string> underwearPaths;
        // The main and off hand weapons as attached, and whether they are on
        // the back, as every rider carries them (riderWeaponAttachment).
        struct HeldWeapon {
            bool attached = false;
            uint8_t inventoryType = 0;
        };
        std::array<HeldWeapon, 2> weapons;
        bool weaponsSheathed = false;
    };
    /// Moves a player's attached weapons into their hands or onto their back;
    /// drawing a mount puts them away and removing it draws them.
    void setRemotePlayerWeaponsSheathed(uint64_t guid, bool sheathed);
    void processDeferredEquipmentQueue();
    struct PreparedEquipmentUpdate {
        uint64_t guid;
        JobGroup::Token token;
        uint32_t instanceId = 0;
        std::vector<std::pair<int, std::string>> regions;
        std::unordered_map<std::string, std::shared_ptr<pipeline::M2Model>> models;
        WornEquipment equipment;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncEquipmentLoad {
        JobGroup::Future<PreparedEquipmentUpdate> future;
    };
    void processAsyncEquipmentResults();
    /// Which runtime texture slot of a player model takes which art. Type 8 is
    /// Skin Extra - the head detail sheet, not the underwear, though on the
    /// models the game shipped the underwear art is what ends up in it.
    struct PlayerTextureSlots { int skin = -1; int hair = -1; int skinExtra = -1; };

    // --- Player spawn queue ---
    struct PendingPlayerSpawn {
        uint64_t guid;
        uint8_t raceId;
        uint8_t genderId;
        uint32_t appearanceBytes;
        uint8_t facialFeatures;
        float x, y, z, orientation;
        JobGroup::Token token;
    };
    struct PreparedPlayerSpawn {
        PendingPlayerSpawn request;
        std::shared_ptr<pipeline::M2Model> model;
        std::string bodySkinPath, skinExtraPath, hairTexturePath, faceLowerPath, faceUpperPath;
        std::vector<std::string> underwearPaths;
        std::unordered_map<std::string, pipeline::BLPImage> textures;
    };
    void processPlayerSpawnQueue();

    // --- GameObject instances ---
    struct GameObjectInstanceInfo {
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        bool isWmo = false;
        /// The GO state its animation was last set for; none before the first.
        std::optional<uint8_t> goState;
    };

    /// States that arrived before their model did, applied when it spawns.
    struct PendingTransportMove {
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    };
    struct PendingTransportRegistration {
        uint64_t guid = 0;
        uint32_t entry = 0;
        uint32_t displayId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
        int retryFrames = 0;  // waiting for the GO's render instance to spawn
    };

    // --- GameObject spawn queue ---
    struct PendingGameObjectSpawn {
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
        JobGroup::Token token;
    };
    void processGameObjectSpawnQueue();

    // --- Async WMO loading for game objects ---
    struct PreparedGameObjectWMO {
        JobGroup::Token token;
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::WMOModel> wmoModel;
        /// An M2 game object read, parsed and skinned on a worker, its
        /// textures decoded beside it. The main thread only uploads it.
        std::shared_ptr<pipeline::M2Model> m2Model;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
        bool valid = false;
        bool isWmo = false;
        std::string modelPath;
        /// Why the worker gave up, for the warning, when valid is false.
        std::string error;
    };
    struct AsyncGameObjectLoad {
        JobGroup::Future<PreparedGameObjectWMO> future;
    };
    /// Display ids whose M2 is being read on a worker. A second object of
    /// the same display waits for that load rather than starting its own.
    /// Objects with a load in flight, and those of them the server removed
    /// before it finished: their model is still kept, but no instance made.
    void finishM2GameObjectLoad(PreparedGameObjectWMO& result);

    // A parsed WMO whose GPU upload is being spread across frames. Uploading a
    // transport in one go cost ~40ms - its textures are large, and that upload
    // has to happen on the main thread even though a worker decoded them. The
    // result is held here because it owns the model and the decoded pixels.
    struct PendingWmoUpload {
        PreparedGameObjectWMO result;
        uint32_t modelId = 0;
    };
    void processPendingWmoUploads();
    void finishWmoSpawn(const PreparedGameObjectWMO& result, uint32_t modelId);
    void processAsyncGameObjectResults();
    struct PendingTransportDoodadBatch {
        uint64_t guid = 0;
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        size_t nextIndex = 0;
        size_t doodadBudget = 0;
        size_t spawnedDoodads = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    };
    static constexpr size_t MAX_TRANSPORT_DOODADS_PER_FRAME = 4;
    void processPendingTransportRegistrations();
    void processPendingTransportDoodads();

    // --- Mount ---
    void processPendingMount();
    void processPendingRemotePlayerMounts();
    // Pending while workers prepare the model; Failed once it is known never
    // to load - logged then, and the caller stops asking.
    enum class ModelLoad { Pending, Ready, Failed };
    ModelLoad loadRemoteMountModel(uint32_t displayId, uint32_t& modelId,
                                   std::string& modelPath, float& riderHeight);
    // Displays whose model failed to load, each reported once.
    // Mounts whose model has no saddle attachment, each reported once.
    void removeRemotePlayerMount(uint64_t guid);

    // --- Remote player forms ---
    // Another player drawn as a creature: a druid's forms, a shaman's ghost
    // wolf, a polymorph - Unit::getFormDisplayId, whatever the server's
    // display ID is when it is not the player's own body.
    //
    // While a form is shown, spawns_->playerInstances_ holds the form's instance, so
    // everything that moves, turns and animates a player moves the form; the
    // character's instance is hidden and kept, dressed, for the way back.
    struct RemotePlayerForm {
        uint32_t displayId = 0;
        uint32_t formInstanceId = 0;
        uint32_t characterInstanceId = 0;
    };
    // Forms with no model to draw them with, asked about once.
    void reconcileRemotePlayerForms();
    void setRemotePlayerFormDisplayId(uint64_t guid, uint32_t displayId);  // 0 = own body
    void processPendingRemotePlayerForms();
    void removeRemotePlayerForm(uint64_t guid);

    // --- Local player GUID exclusion ---

    // --- Weapon model ID counter ---
    // Weapon model ids share CharacterRenderer's models map with NPC
    // composites, which are keyed by creature displayId (1..~35000). Starting
    // at 1000 collided with loaded NPC models once enough weapon reloads
    // advanced the counter - attachWeapon then instanced an NPC mesh as the
    // weapon. Reserve a range no displayId can reach.

    // --- Spawn internal methods ---
    void spawnOnlineCreature(uint64_t guid, uint32_t displayId,
                             float x, float y, float z, float orientation, float scale = 1.0f);
    void spawnOnlinePlayer(uint64_t guid, uint8_t raceId, uint8_t genderId,
                           uint32_t appearanceBytes, uint8_t facialFeatures,
                           float x, float y, float z, float orientation);
    void setOnlinePlayerEquipment(uint64_t guid, const WornEquipment& equipment);
    void spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId,
                               float x, float y, float z, float orientation, float scale = 1.0f);
    struct AssetCaches {
    std::unordered_map<uint32_t, CreatureDisplayData> displayDataMap_;  // displayId → display data
    std::unordered_map<uint32_t, HumanoidDisplayExtra> humanoidExtraMap_;  // extraDisplayId → humanoid data
    std::unordered_map<uint32_t, std::string> modelIdToPath_;   // modelId → M2 path (from CreatureModelData.dbc)
    std::unordered_map<uint32_t, float> modelIdToScale_;        // modelId → CreatureModelData.ModelScale
    std::unordered_map<uint32_t, uint16_t> hairGeosetMap_;
    std::unordered_map<uint32_t, FacialHairGeosets> facialHairGeosetMap_;
    bool creatureLookupsBuilt_ = false;
    std::unordered_map<uint64_t, std::string> charSectionsCache_;
    bool charSectionsCacheBuilt_ = false;
    std::unordered_map<uint32_t, std::string> gameObjectDisplayIdToPath_;
    bool gameObjectLookupsBuilt_ = false;
    std::unordered_map<uint32_t, bool> modelIdIsWolfLike_;
    std::unordered_map<std::string, std::shared_ptr<pipeline::M2Model>> attachmentModelData_;
    std::unordered_map<std::string, uint32_t> attachmentModelIds_;
    std::unordered_map<uint32_t, uint32_t> displayIdModelCache_;
    std::unordered_set<uint32_t> displayIdTexturesApplied_;
    mutable std::unordered_set<uint32_t> warnedMissingDisplayDataIds_;
    mutable std::unordered_set<uint32_t> warnedMissingModelPathIds_;
    mutable int humanoidDisplayCanaryCount_ = 0;
    int npcHeadDetailCanaryCount_ = 0;
    uint32_t gryphonDisplayId_ = 0;
    uint32_t wyvernDisplayId_ = 0;
    std::unordered_set<uint32_t> nonRenderableCreatureDisplayIds_;
    std::unordered_map<uint32_t, uint32_t> playerModelCache_;
    std::unordered_map<uint32_t, PlayerTextureSlots> playerTextureSlotsByModelId_;
    std::unordered_map<uint32_t, std::shared_ptr<pipeline::M2Model>> preparedPlayerModels_;
    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdModelCache_;
    std::unordered_set<uint32_t> gameObjectDisplayIdFailedCache_;
    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdWmoCache_;
    std::unordered_set<uint32_t> unloadableMountDisplayIds_;
    std::unordered_set<uint32_t> seatlessMountDisplayIds_;
    std::unordered_set<uint32_t> undrawableFormDisplayIds_;
    };
    AssetCaches assets_;

    // Renderer model caches survive table/realm changes. Their identities
    // must survive too: resetting an allocator with AssetCaches lets a new
    // asset acquire the ID of another mesh still resident on the GPU.
    uint32_t nextCreatureModelId_ = 5000;
    uint32_t nextPlayerModelId_ = 60000;
    uint32_t nextGameObjectModelId_ = 20000;
    uint32_t nextGameObjectWmoModelId_ = 40000;
    uint32_t nextWeaponModelId_ = 0x40000000u;

public:
    /// Owned by MapScope. No clearing list: queues, GUID state and decoded
    /// results all disappear together, after its JobGroup has joined.
    struct SpawnState {
        explicit SpawnState(JobGroup& jobs) : jobs_(jobs) {}
        JobGroup& jobs_;
    /// Presented entities held across a reconnect, not yet shown again
    /// (holdPresented).
    std::unordered_set<uint64_t> held_;
    std::unordered_map<uint64_t, uint32_t> creatureInstances_;  // guid → render instanceId
    std::unordered_map<uint64_t, uint32_t> creatureModelIds_;   // guid → loaded modelId
    std::unordered_map<uint64_t, uint32_t> creatureDisplayIds_; // guid → active displayId
    std::unordered_map<uint64_t, glm::vec3> creatureRenderPosCache_; // guid → last synced render position
    std::unordered_map<uint64_t, bool> creatureWasMoving_;
    std::unordered_map<uint64_t, bool> creatureWasSwimming_;
    std::unordered_map<uint64_t, bool> creatureWasFlying_;
    std::unordered_map<uint64_t, bool> creatureWasWalking_;
    std::unordered_map<uint64_t, bool> creatureSwimmingState_;
    std::unordered_map<uint64_t, bool> creatureWalkingState_;
    std::unordered_map<uint64_t, bool> creatureFlyingState_;
    std::unordered_map<uint64_t, uint32_t> creatureActiveEmotes_;
    std::unordered_map<uint64_t, bool> creatureWasStealthed_;
    uint32_t stealthSyncFrameCounter_ = 0;  // throttles syncCreatureStealthVisuals()
    std::unordered_set<uint64_t> creatureWeaponsAttached_;
    std::unordered_map<uint64_t, uint8_t> creatureWeaponAttachAttempts_;
    std::vector<AsyncCreatureLoad> asyncCreatureLoads_;
    std::unordered_set<uint32_t> asyncCreatureDisplayLoads_;
    std::unordered_set<uint64_t> deadCreatureGuids_;
    std::unordered_map<uint32_t, std::unordered_map<std::string, pipeline::BLPImage>> displayIdPredecodedTextures_;
    std::deque<PendingCreatureSpawn> pendingCreatureSpawns_;
    std::unordered_set<uint64_t> pendingCreatureSpawnGuids_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>
        creatureSpawnRetryDeadlines_;
    std::unordered_map<uint64_t, int> creatureSpawnRetryWindowsUsed_;
    std::unordered_set<uint64_t> creaturePermanentFailureGuids_;
    std::vector<AsyncNpcCompositeLoad> asyncNpcCompositeLoads_;
    std::unordered_map<uint64_t, uint32_t> playerInstances_;  // guid → render instanceId
    std::unordered_map<uint64_t, OnlinePlayerAppearanceState> onlinePlayerAppearance_;
    std::unordered_map<uint64_t, WornEquipment> pendingOnlinePlayerEquipment_;
    std::deque<std::pair<uint64_t, WornEquipment>> deferredEquipmentQueue_;
    std::vector<AsyncEquipmentLoad> asyncEquipmentLoads_;
    const PreparedEquipmentUpdate* preparedEquipmentUpdate_ = nullptr;
    std::unordered_map<std::string, JobGroup::Future<PreparedEquipmentUpdate>> asyncAttachmentLoads_;
    std::unordered_map<std::string, pipeline::BLPImage> preparedAttachmentTextures_;
    std::deque<PendingPlayerSpawn> pendingPlayerSpawns_;
    std::unordered_set<uint64_t> pendingPlayerSpawnGuids_;
    std::vector<JobGroup::Future<PreparedPlayerSpawn>> asyncPlayerLoads_;
    const PreparedPlayerSpawn* preparedPlayerSpawn_ = nullptr;
    std::unordered_map<uint64_t, GameObjectInstanceInfo> gameObjectInstances_;
    std::unordered_map<uint64_t, uint8_t> gameObjectPendingState_;
    std::unordered_map<uint64_t, PendingTransportMove> pendingTransportMoves_;
    std::deque<PendingTransportRegistration> pendingTransportRegistrations_;
    bool testTransportSetup_ = false;
    std::deque<PendingGameObjectSpawn> pendingGameObjectSpawns_;
    std::unordered_set<uint32_t> asyncGameObjectM2Displays_;
    std::unordered_set<uint64_t> asyncGameObjectGuids_;
    std::vector<PendingWmoUpload> pendingWmoUploads_;
    std::vector<AsyncGameObjectLoad> asyncGameObjectLoads_;
    std::vector<PendingTransportDoodadBatch> pendingTransportDoodadBatches_;
    uint32_t mountInstanceId_ = 0;
    uint32_t mountModelId_ = 0;
    uint32_t pendingMountDisplayId_ = 0;
    std::unordered_map<uint64_t, RemotePlayerMount> remotePlayerMounts_;
    std::unordered_map<uint64_t, RemotePlayerLocomotion> remotePlayerLocomotion_;
    std::unordered_map<uint64_t, uint32_t> pendingRemotePlayerMounts_;
    std::unordered_map<uint64_t, RemotePlayerForm> remotePlayerForms_;
    std::unordered_map<uint64_t, uint32_t> pendingRemotePlayerForms_;
    uint64_t spawnedPlayerGuid_ = 0;
    };
    void bindMap(SpawnState* state) { spawns_ = state; }
    bool hasMap() const { return spawns_ != nullptr; }

private:
    SpawnState* spawns_ = nullptr;

};

} // namespace core
} // namespace wowee
