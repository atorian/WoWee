#pragma once

#include "pipeline/blp_loader.hpp"

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
    // Spell visual effects - transient M2 instances spawned by SMSG_PLAY_SPELL_VISUAL/IMPACT
    struct SpellVisualInstance {
        uint32_t instanceId;
        float elapsed;
        float duration;  // per-instance lifetime in seconds (from M2 anim or default)
        bool isPrecast;  // true for precast effects (removed on cancel/interrupt)
        uint32_t attachmentId;  // character attachment point to track (0=none/static)
        uint32_t attachInstanceId;  // CharacterRenderer instance the attachment belongs to
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
    struct DeferredPlay {
        enum class Kind { Precast, Cast, Impact } kind = Kind::Cast;
        uint32_t visualId = 0;
        glm::vec3 position{0.0f};
        uint32_t castTimeMs = 0;
        uint32_t attachInstanceId = 0;
        uint32_t modelId = 0;
        float age = 0.0f;
    };
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

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    std::vector<PhysicalProjectile> physicalProjectiles_;
    std::unordered_map<uint32_t, std::string> spellVisualPrecastPath_; // visualId → precast M2 path
    std::unordered_map<uint32_t, std::string> spellVisualCastPath_;   // visualId → cast M2 path
    std::unordered_map<uint32_t, std::string> spellVisualImpactPath_; // visualId → impact M2 path
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    bool spellVisualDbcLoaded_ = false;
    static constexpr float SPELL_VISUAL_MAX_DURATION = 5.0f;
    static constexpr float SPELL_VISUAL_DEFAULT_DURATION = 2.0f;

    // Determine character attachment point from model path keywords
    static uint32_t classifyAttachmentId(const std::string& modelPath);

    // Apply height offset based on model path keywords (Hand → hands, Chest → chest, Base → ground)
    static glm::vec3 applyEffectHeightOffset(const glm::vec3& basePos, const std::string& modelPath);

    // ---- Aura state visuals (spell_visual_state.cpp) ----
    struct StateKit {
        struct Effect {
            std::string path;
            /// The M2 attachment the effect hangs from, by the kit column it
            /// came from (kKitEffectAttachments in spell_visual_state.cpp).
            uint32_t attachmentId = 0;
            float scale = 1.0f;  // SpellVisualEffectName.Scale
            /// SpellVisualEffectName.Min/MaxAllowedScale: the bounds of the
            /// size the effect is drawn at, its unit's size included.
            float minScale = 0.0f;
            float maxScale = std::numeric_limits<float>::max();
        };
        std::vector<Effect> effects;
        glm::vec3 tint{1.0f};    // CharProc 1 (Color); white when it has none
        float alpha = 1.0f;      // CharProc 14 (UpdateAlphaEffectAlpha)
    };
    struct StateEffect {
        uint32_t visualId = 0;
        size_t effectIndex = 0;     // into StateKit::effects
        uint32_t modelId = 0;
        uint32_t instanceId = 0;    // M2 instance; 0 while the model loads
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
    /// Where an effect on this unit is drawn: its attachment's world matrix,
    /// so it turns and sways with the body, times the effect's own scale,
    /// the total held within the effect's allowed sizes.
    bool stateEffectTransform(uint32_t charInstanceId, const StateKit::Effect& effect,
                              glm::mat4& out);
    std::unordered_map<uint32_t, StateKit> stateKits_;  // visualId -> its StateKit
    bool stateKitsLoaded_ = false;
    std::unordered_map<uint64_t, UnitState> unitStates_;  // by unit guid
};

} // namespace rendering
} // namespace wowee
