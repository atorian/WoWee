#pragma once

#include "pipeline/blp_loader.hpp"
#include "rendering/spell_visual_kit.hpp"

#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; struct M2Model; }
namespace rendering {

class M2Renderer;
class Renderer;
class CharacterRenderer;

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem() = default;

    // Initialize with references to the M2 renderer and parent renderer
    void initialize(M2Renderer* m2Renderer, Renderer* renderer);
    void shutdown();

    // Spawn a spell visual at a world position.
    // useImpactKit=false → CastKit path; useImpactKit=true → ImpactKit path
    // attachInstanceId: the CASTER's CharacterRenderer instance for hand/chest/
    // head bone tracking (0 = static effect at worldPosition). Effects used to
    // attach to the local player unconditionally, so every nearby unit's cast
    // kit landed on the player's hands.
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false, uint32_t attachInstanceId = 0);

    // Spawn a precast visual effect at a world position.
    // castTimeMs: server cast time in milliseconds (0 = use anim duration).
    // attachInstanceId: see playSpellVisual.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t castTimeMs = 0, uint32_t attachInstanceId = 0);

    // Launch a physical weapon projectile (arrow, bullet, or thrown item)
    // without invoking the spell visual pipeline.
    void playPhysicalProjectile(const std::string& modelPath,
                                const std::string& texturePath,
                                const glm::vec3& start,
                                const glm::vec3& end,
                                float duration,
                                bool spin);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // Remove all active precast visual instances (cast canceled/interrupted).
    void cancelAllPrecastVisuals();

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

    /// Loads, off the main thread, every effect model these visuals use -
    /// precast, cast and impact - so the character's own spells never load
    /// one in the frame they are cast. Kept resident from then on, and loaded
    /// again in the background if a world change clears them.
    void preloadVisuals(const std::vector<uint32_t>& visualIds);

    /// A unit, and the spell visuals of the auras it has now.
    struct UnitStateVisuals {
        uint64_t guid = 0;
        uint32_t instanceId = 0;          // its CharacterRenderer instance
        std::vector<uint32_t> visualIds;  // SpellVisual ids, any order
    };
    /// What a unit looks like for as long as an aura lasts: the StateKit of
    /// the aura's spell visual. Its effect models hang on the unit - the
    /// shadow at a Shadowform priest's feet, a Power Word: Shield bubble -
    /// and its character procs tint the body and make it translucent.
    ///
    /// `units` is every unit with auras that is drawn now. One that has gone
    /// from it, or whose list no longer names a visual, loses that look.
    void syncStateVisuals(const std::vector<UnitStateVisuals>& units);

private:
    enum class KitKind : uint8_t { Precast, Cast, Impact };
    /// Where a kit is played: on a unit (its CharacterRenderer instance), or
    /// at a point for the World column and for a unit not drawn as a character.
    struct KitPlacement {
        glm::vec3 worldPosition{0.0f};
        uint32_t unitInstanceId = 0;
    };
    /// One effect model of a kit, playing.
    struct ActiveEffect {
        uint32_t instanceId = 0;
        uint32_t kitId = 0;
        size_t effectIndex = 0;   // into kits_[kitId].effects
        KitPlacement at;
        glm::mat4 lastTransform{1.0f};
        float elapsed = 0.0f;
        float lifetime = 0.0f;    // seconds until it finishes
        bool isPrecast = false;   // ended early when the cast is cancelled
    };
    /// A one-shot whose model has no Stand to speak of still shows this long.
    static constexpr float kOneShotFallbackSeconds = 1.0f;
    struct VisualKits {
        uint32_t precast = 0, cast = 0, impact = 0;
    };

    struct PhysicalProjectile {
        uint32_t instanceId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 rotation{0.0f};
        float elapsed = 0.0f;
        float duration = 0.0f;
        bool spin = false;
    };

    void loadSpellVisualDbc();

    // ---- Background model loading ----
    //
    // A spell's effect model was read, parsed and its textures decoded on the
    // main thread the first time the spell was seen - and again after the
    // model reaper dropped it a minute later. A mage's Blink cost 15-60 ms of
    // frame every time. Models now load on an I/O worker; a play request for
    // one that is not resident waits for it and plays when it lands, if the
    // cast is still current.
    struct PreparedModel {
        bool ok = false;
        std::shared_ptr<pipeline::M2Model> model;
        std::unordered_map<std::string, pipeline::BLPImage> textures;
    };
    struct PendingLoad {
        std::string path;
        uint32_t modelId = 0;
        std::future<PreparedModel> future;
    };
    /// One kit effect waiting for its model to load.
    struct DeferredPlay {
        KitKind kind = KitKind::Cast;
        uint32_t kitId = 0;
        size_t effectIndex = 0;
        KitPlacement at;
        uint32_t castTimeMs = 0;
        uint32_t modelId = 0;
        float age = 0.0f;
    };
    uint32_t kitFor(uint32_t visualId, KitKind kind);
    void playKit(uint32_t kitId, KitKind kind, const KitPlacement& at, uint32_t castTimeMs);
    void playKitEffect(uint32_t kitId, size_t effectIndex, KitKind kind, const KitPlacement& at,
                       uint32_t castTimeMs);
    /// Where a kit effect is drawn now: its attachment's whole matrix on the
    /// unit (or the unit's own where its model lacks the point) at the
    /// effect's scale, or its point in the world. False when the unit is gone.
    bool kitEffectTransform(const KitPlacement& at, const SpellKitEffect& effect, glm::mat4& out) const;
    /// The model id for this path, assigned on first use; 0 when the pool is
    /// used up.
    uint32_t modelIdFor(const std::string& path);
    /// True when the model is resident. Otherwise starts loading it in the
    /// background (unless it failed before) and returns false.
    bool ensureModel(const std::string& path, uint32_t modelId);
    void finishPendingLoads();
    void replayDeferredPlays(float deltaTime);
    std::vector<PendingLoad> pendingLoads_;
    std::unordered_set<uint32_t> pendingModelIds_;
    std::vector<DeferredPlay> deferredPlays_;
    std::unordered_set<std::string> preloadPaths_;
    float preloadCheckTimer_ = 0.0f;

    M2Renderer* m2Renderer_ = nullptr;
    Renderer* renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<ActiveEffect> activeEffects_;
    std::vector<PhysicalProjectile> physicalProjectiles_;
    std::unordered_map<uint32_t, SpellVisualKit> kits_;       // kitId → the effects it plays
    std::unordered_map<uint32_t, VisualKits> visualKits_;     // visualId → its three kits
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    bool spellVisualDbcLoaded_ = false;

    // ---- Aura state visuals (spell_visual_state.cpp) ----
    struct StateKit {
        std::vector<SpellKitEffect> effects;
        glm::vec3 tint{1.0f};    // CharProc 1 (Color); white when it has none
        float alpha = 1.0f;      // CharProc 14 (UpdateAlphaEffectAlpha)
        /// Kit flag 0x20: its models loop their Stand. Otherwise they play
        /// Stand once and then their Hold, over and over, if they have one.
        bool loopStand = false;
    };
    struct StateEffect {
        uint32_t visualId = 0;
        size_t effectIndex = 0;     // into StateKit::effects
        uint32_t modelId = 0;
        uint32_t instanceId = 0;    // M2 instance; 0 while the model loads
        glm::vec3 worldPosition{0.0f};  // where a World-column effect stays
        glm::mat4 lastTransform{1.0f};
        float lastAlpha = 1.0f;     // the unit's own alpha, which the effect shares
    };
    struct UnitState {
        uint32_t instanceId = 0;
        std::vector<uint32_t> visualIds;  // sorted, only ones with a kit
        std::vector<StateEffect> effects;
        glm::vec3 tint{1.0f}, targetTint{1.0f};
        float alpha = 1.0f, targetAlpha = 1.0f;
    };
    void loadStateKits();
    void setUnitStateKits(UnitState& state, std::vector<uint32_t> visualIds);
    void removeStateEffects(UnitState& state);
    void updateStateVisuals(float deltaTime);
    /// Takes every state visual down and gives the bodies their own look back.
    void resetStateVisuals();
    std::unordered_map<uint32_t, StateKit> stateKits_;  // visualId -> its StateKit
    bool stateKitsLoaded_ = false;
    std::unordered_map<uint64_t, UnitState> unitStates_;  // by unit guid
};

} // namespace rendering
} // namespace wowee
