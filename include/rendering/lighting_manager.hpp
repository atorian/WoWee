#pragma once

#include <functional>
#include <vector>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class DBCFile; class AssetManager; }

namespace rendering {

/**
 * Time-of-day lighting parameters sampled from DBC curves
 */
struct LightingParams {
    glm::vec3 ambientColor{0.4f, 0.4f, 0.5f};      // Fill lighting
    glm::vec3 diffuseColor{1.0f, 0.95f, 0.8f};     // Directional sun color
    glm::vec3 directionalDir{0.0f, -1.0f, 0.5f};   // Sun direction (normalized)

    glm::vec3 fogColor{0.5f, 0.6f, 0.7f};          // Fog color
    float fogStart = 100.0f;                        // Fog start distance
    float fogEnd = 1000.0f;                         // Fog end distance
    float fogDensity = 0.001f;                      // Fog density

    glm::vec3 skyTopColor{0.5f, 0.7f, 1.0f};       // Sky zenith color
    glm::vec3 skyMiddleColor{0.7f, 0.85f, 1.0f};   // Sky horizon color
    glm::vec3 skyBand1Color{0.9f, 0.95f, 1.0f};    // Sky band 1
    glm::vec3 skyBand2Color{1.0f, 0.98f, 0.9f};    // Sky band 2
    glm::vec3 skySmogColor{0.5f, 0.6f, 0.7f};    // Horizon band, LightIntBand channel 6

    /// The sun's and the moon's disc and glare, LightIntBand channel 9. Not the
    /// light the sun casts (diffuseColor): Tirisfal's sun lights the ground a
    /// cool white and is drawn pale green; Duskwood's is blue, Teldrassil's pink.
    glm::vec3 sunColor{1.0f, 0.97f, 0.87f};
    /// The clouds' colours, channels 10 to 12, as WoW.exe lights its cloud
    /// texture (0x7efae0): the base, plus the fill by how thin the cloud is,
    /// plus the sunlit colour where a cloud faces the sun or the moon. The
    /// sunlit colour is the zone's tint - green over Tirisfal, orange over
    /// Durotar - and the volumetric air's forward glow takes it too.
    glm::vec3 cloudSunlitColor{1.0f, 0.78f, 0.54f};
    glm::vec3 cloudFillColor{0.55f, 0.55f, 0.6f};
    glm::vec3 cloudBaseColor{0.35f, 0.35f, 0.4f};

    float cloudDensity = 0.3f;                      // Cloud density/opacity
    float horizonGlow = 0.3f;                       // Horizon glow intensity

    // Value initialization uses the fallback member defaults above. A weighted
    // sum must start at zero, otherwise every zone gains a second light/sky.
    static LightingParams blendAccumulator() {
        return {
            .ambientColor = glm::vec3(0.0f),
            .diffuseColor = glm::vec3(0.0f),
            .directionalDir = glm::vec3(0.0f),
            .fogColor = glm::vec3(0.0f),
            .fogStart = 0.0f, .fogEnd = 0.0f, .fogDensity = 0.0f,
            .skyTopColor = glm::vec3(0.0f),
            .skyMiddleColor = glm::vec3(0.0f),
            .skyBand1Color = glm::vec3(0.0f),
            .skyBand2Color = glm::vec3(0.0f),
            .skySmogColor = glm::vec3(0.0f),
            .sunColor = glm::vec3(0.0f),
            .cloudSunlitColor = glm::vec3(0.0f),
            .cloudFillColor = glm::vec3(0.0f),
            .cloudBaseColor = glm::vec3(0.0f),
            .cloudDensity = 0.0f, .horizonGlow = 0.0f
        };
    }
};

/**
 * Light set keyframe for time-of-day interpolation
 */
struct LightKeyframe {
    uint32_t time;  // Time in minutes since midnight (0-1439)

    // Colors stored as RGB int tuples (0-255) in DBC
    glm::vec3 ambientColor;
    glm::vec3 diffuseColor;
    glm::vec3 fogColor;
    glm::vec3 skyTopColor;
    glm::vec3 skyMiddleColor;
    glm::vec3 skyBand1Color;
    glm::vec3 skyBand2Color;

    float fogStart;
    float fogEnd;
    float fogDensity;
    float cloudDensity;
    float horizonGlow;
};

/**
 * Light volume from Light.dbc (spatial lighting)
 */
struct LightVolume {
    uint32_t lightId = 0;
    uint32_t mapId = 0;
    glm::vec3 position{0.0f};  // World position (note: DBC stores as x,z,y!)
    float innerRadius = 0.0f;   // Full weight radius
    float outerRadius = 0.0f;   // Fade-out radius

    // LightParams IDs for different conditions, in Light.dbc's column order.
    // Zero where the volume has nothing special for that case.
    uint32_t lightParamsId = 0;                 // Clear
    uint32_t lightParamsIdUnderwater = 0;       // Clear, camera under water
    uint32_t lightParamsIdStorm = 0;            // Rain or storm
    uint32_t lightParamsIdStormUnderwater = 0;  // Storm, camera under water
    // More variants exist for death etc.
};

/**
 * Color band with time-of-day keyframes
 */
struct ColorBand {
    uint8_t numKeyframes = 0;
    uint16_t times[16];        // Time keyframes (half-minutes since midnight)
    glm::vec3 colors[16];      // Color values (RGB 0-1)
};

/**
 * Float band with time-of-day keyframes
 */
struct FloatBand {
    uint8_t numKeyframes = 0;
    uint16_t times[16];        // Time keyframes (half-minutes since midnight)
    float values[16];          // Float values
};

/**
 * LightParams profile with 18 color bands + 6 float bands
 */
struct LightParamsProfile {
    uint32_t lightParamsId = 0;
    uint32_t lightSkyboxId = 0;

    // 18 color channels (IntBand)
    //
    // Read off the file rather than from the order they are usually listed in.
    // The sky gradient anchors the rest: for Stormwind at noon, channels 2 to 5
    // are (0,31,73), (58,162,207), (153,220,245), (175,218,224) - a deep blue
    // overhead paling to the horizon, which can only be the sky and fixes every
    // index around it.
    //
    // Channel 0 is not the ambient. Across zones it is the warm, bright one -
    // (199,168,134) over Dun Morogh's snow, (255,224,169) in Teldrassil,
    // (255,136,0) in Stormwind - while channel 1 is the dark cool one that
    // carries the zone's cast: (31,82,125) in Dun Morogh, violet (125,72,130)
    // in Teldrassil and Winterspring. A scene ambient multiplies every surface,
    // so reading channel 0 into it put Stormwind's orange over the whole city
    // and left it looking like Durotar, whose channel 0 is (255,204,148).
    //
    // Channel 7 is the fog. Channel 6 is the sky's smog layer and is a neutral
    // grey - (180,180,180) at Stormwind noon - which is the pale grey the fog
    // blend further down was written to work around.
    enum ColorChannel {
        DIFFUSE_COLOR = 0,
        AMBIENT_COLOR = 1,
        SKY_TOP_COLOR = 2,
        SKY_MIDDLE_COLOR = 3,
        SKY_BAND1_COLOR = 4,
        SKY_BAND2_COLOR = 5,
        SKY_SMOG_COLOR = 6,
        FOG_COLOR = 7,
        // The sun's and moon's disc and glare (WoW.exe 0x7f36f7). Across the
        // file a near-white - (255,247,222) over most of the world - turned
        // pale green in Tirisfal and Felwood, blue in Duskwood, pink in
        // Teldrassil.
        SUN_COLOR = 9,
        // The clouds (0x7efae0): sunlit, fill and base. 10 is each zone's tint
        // at full strength - orange over Durotar, green over Tirisfal, violet
        // over Teldrassil.
        CLOUD_SUNLIT_COLOR = 10,
        CLOUD_FILL_COLOR = 11,
        CLOUD_BASE_COLOR = 12,
        // ... more channels exist (ocean, river)
        COLOR_CHANNEL_COUNT = 18
    };

    ColorBand colorBands[COLOR_CHANNEL_COUNT];

    // 6 float channels (FloatBand)
    enum FloatChannel {
        FOG_END = 0,
        FOG_START_SCALAR = 1,  // Multiplier for fog start
        // Channel 2 is not a density. Measured over every band in the file,
        // 98% of its 2509 samples are exactly 1.0 - it is the switch for how
        // much the sun and moon show through cloud, and reading it as cloud
        // density left almost every zone under solid overcast at all hours,
        // which is what hid the skybox.
        CELESTIAL_GLOW_THROUGH = 2,
        // Channel 3 is the curve that actually varies: 0 to 5, mean 0.50.
        CLOUD_DENSITY = 3,
        // No channel is a fog density. One was declared as a second name for
        // channel 3, and the cloud curve dimmed the lens flare and the stars
        // twice, once as cloud and once as fog.
        // ... more channels
        FLOAT_CHANNEL_COUNT = 6
    };

    FloatBand floatBands[FLOAT_CHANNEL_COUNT];
};

/**
 * WoW DBC-driven lighting manager
 *
 * Implements WotLK's time-of-day lighting system:
 * - Loads Light.dbc, LightParams.dbc, LightIntBand.dbc, LightFloatBand.dbc
 * - Samples lighting curves based on time-of-day
 * - Interpolates between keyframes
 * - Provides lighting parameters for rendering
 */
class LightingManager {
public:
    LightingManager();
    ~LightingManager();

    /**
     * Initialize lighting system and load DBCs
     */
    bool initialize(pipeline::AssetManager* assetManager);

    /**
     * Update lighting for current time and player position
     * @param deltaTime Elapsed game-frame seconds, including light/sky fades
     * @param playerPos Player world position
     * @param mapId Current map ID
     * @param gameTime Optional game time in seconds (use -1 for real time)
     * @param weatherIntensity The weather's intensity, 0-1: each volume's light
     *        is its clear row blended toward its storm row by
     *        min(1, 4 x intensity), as WoW.exe blends them (0x7ec220).
     * @param isUnderwater Whether the camera is under water
     *
     * Note: WoW uses server-sent game time, not local PC time.
     * Pass gameTime from SMSG_LOGIN_SETTIMESPEED or similar.
     */
    void update(float deltaTime, const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId,
                float gameTime = -1.0f,
                float weatherIntensity = 0.0f, bool isUnderwater = false);

    /**
     * Get current lighting parameters
     */
    [[nodiscard]] const LightingParams& getLightingParams() const { return currentParams_; }

    /// Whether the camera is inside a WMO interior group. Indoors the sky
    /// models fade out. The frame's light stays the outdoor one: it lights
    /// what is seen through a door, and each model inside an interior group
    /// takes its own light instead - see interior_light.glsli.
    void setIndoors(bool indoors) { isIndoors_ = indoors; }
    [[nodiscard]] bool isIndoors() const { return isIndoors_; }
    /// Distance-fog density, 0 (off) through 2. The renderer applies it after
    /// zone/time smoothing; getLightingParams() retains the authored profile.
    /// Outdoor haze uses 0.8 as its calibrated reference appearance.
    void setFogStrength(float strength) { fogStrength_ = strength; }
    [[nodiscard]] float getFogStrength() const { return fogStrength_; }
    /// How strongly outdoor haze thins with height: 0 uniform, 1 calibrated.
    void setFogHeightInfluence(float value) { fogHeightInfluence_ = glm::clamp(value, 0.0f, 2.0f); }
    [[nodiscard]] float getFogHeightInfluence() const { return fogHeightInfluence_; }

    /// Integrate distant-air illumination along the view ray. Independent of density.
    void setAerialRayMarching(bool enabled) { aerialRayMarching_ = enabled; }
    [[nodiscard]] bool getAerialRayMarching() const { return aerialRayMarching_; }

    /**
     * Get current time of day (0.0-1.0)
     */
    [[nodiscard]] float getTimeOfDay() const { return timeOfDay_; }

    /// The same hour, 0-24: the one the light bands are sampled at, and the
    /// sky's, in every zone alike.
    [[nodiscard]] float getTimeOfDayHours() const { return timeOfDay_ * 24.0f; }

    /// LightSkybox.dbc's flag for a sky model drawn over the client's own sky
    /// rather than instead of it - the auroras, Dragonblight's scarlet sky,
    /// the Death Knight fire sky, the Wintergrasp battlefield haze.
    static constexpr uint32_t LIGHT_SKYBOX_COMBINE_PROCEDURAL = 0x2;

    /// One of the original client's sky models, and how much of it is up.
    struct SkyboxLayer {
        uint32_t id = 0;       ///< the LightSkybox row
        std::string path;
        uint32_t flags = 0;    ///< LightSkybox.flags
        float weight = 0.0f;   ///< 0..1: the share of the lights here that name it

        /// Whether this sky, once its model is loaded, stands in for the
        /// client's own sky - the star dome, sun, moons and clouds - so they
        /// are not drawn under it.
        ///
        /// WoW.exe's sky render (0x7f09b0) skips that whole group for a
        /// blended skybox slot that is loaded, above 0.99 and without
        /// LIGHT_SKYBOX_COMBINE_PROCEDURAL (the flag is read at 0x7f3877).
        /// Anything less - a sky still fading in, two crossfading at a
        /// border, or one that sets the flag - is drawn over the client's
        /// sky, after it.
        [[nodiscard]] bool hidesProceduralSky() const {
            return weight > 0.99f && (flags & LIGHT_SKYBOX_COMBINE_PROCEDURAL) == 0;
        }
    };
    /// Every sky model the lights around the player name, heaviest first, at
    /// the weights those lights have - the same falloff the sky colours blend
    /// by, smoothed the same way. A zone's sky fades out across its edge as
    /// the next one's fades in, rather than one swapping for the other.
    ///
    /// One layer per LightSkybox row, as WoW.exe keeps its slots (0x7ed713):
    /// lights naming the same row add up, and two rows naming one model stay
    /// apart, since their flags can differ.
    [[nodiscard]] const std::vector<SkyboxLayer>& getSkyboxLayers() const { return skyboxLayers_; }

    /// Whether the sky models up here stand in for the client's own sky this
    /// frame, so its stars, sun, moons and clouds are left out: one layer that
    /// hidesProceduralSky and whose model is loaded. `isLoaded` answers that
    /// for a LightSkybox row; a sky whose model failed to load hides nothing,
    /// as in WoW.exe, which asks the slot's model first (0x7f0b14).
    [[nodiscard]] bool skyModelsHideProceduralSky(
        const std::function<bool(uint32_t skyboxId)>& isLoaded) const;

    /**
     * Manually set time of day for testing
     */
    void setTimeOfDay(float tod) { timeOfDay_ = tod; manualTime_ = true; }

    /**
     * Use real time for day/night cycle
     */
    void useRealTime(bool use) { manualTime_ = !use; }

private:
    /**
     * Load Light.dbc
     */
    bool loadLightDbc(pipeline::AssetManager* assetManager);

    /**
     * Load LightParams.dbc for zone→light mapping
     */
    bool loadLightParamsDbc(pipeline::AssetManager* assetManager);

    bool loadLightSkyboxDbc(pipeline::AssetManager* assetManager);

    /**
     * Load LightIntBand.dbc and LightFloatBand.dbc for time curves
     */
    bool loadLightBandDbcs(pipeline::AssetManager* assetManager);

    /**
     * A light and its share of what a point is lit by.
     */
    struct WeightedVolume {
        const LightVolume* volume = nullptr;
        float weight = 0.0f;
    };

    /**
     * The lights that make a point's light, each with its share once all are
     * laid down as the client lays them (light_volume_order.hpp): the map's
     * global light first, then the volumes the point stands in, farthest
     * first. The shares add up to one when the map has a global light.
     */
    [[nodiscard]] std::vector<WeightedVolume> findLightVolumes(const glm::vec3& playerPos, uint32_t mapId) const;

    /**
     * Convert DBC BGR color to RGB vec3
     */
    [[nodiscard]] glm::vec3 dbcColorToVec3(uint32_t dbcColor) const;


    // Light volumes by map
    std::map<uint32_t, std::vector<LightVolume>> lightVolumesByMap_;
    /// Each map's global light: its Light.dbc row at the origin, which the
    /// volumes are laid over and which lights it where none reaches.
    std::map<uint32_t, LightVolume> globalLightByMap_;
    /// Maps with no global light, reported once each.
    mutable std::set<uint32_t> mapsWithoutGlobalLight_;

    // LightParams profiles by ID
    std::map<uint32_t, LightParamsProfile> lightParamsProfiles_;
    /// LightSkybox rows by id.
    struct SkyboxRow {
        std::string path;
        uint32_t flags = 0;
    };
    std::map<uint32_t, SkyboxRow> lightSkyboxes_;
    /// LightSkybox ids a LightParams row names and the file lacks, reported once.
    std::set<uint32_t> missingSkyboxIds_;

    // Current state
    LightingParams currentParams_;
    std::vector<WeightedVolume> activeVolumes_;
    glm::vec3 currentPlayerPos_{0.0f};
    float timeOfDay_ = 0.5f;  // Start at noon
    std::vector<SkyboxLayer> skyboxLayers_;
    bool isIndoors_ = false;
    float fogStrength_ = 0.4f;
    float fogHeightInfluence_ = 1.0f;
    bool aerialRayMarching_ = false;

    // Last values the sky diagnostic reported, so it prints on a change
    // rather than every frame. See LightingManager::update.
    /// The map the volume list was last named for, so it is named once per map
    /// rather than once per frame. findLightVolumes is const.
    mutable uint32_t diagLoggedMapId_ = 0xFFFFFFFFu;
    uint32_t diagZoneId_ = 0xFFFFFFFFu;
    uint32_t diagCallsSinceLog_ = 0;
    uint32_t diagFirstVolume_ = 0xFFFFFFFFu;
    uint32_t diagSecondVolume_ = 0xFFFFFFFFu;
    float diagHours_ = -1.0f;
    float diagSkyLuma_ = -1.0f;
    std::string diagSkyboxPath_ = "\x01";
    bool manualTime_ = false;
    bool initialized_ = false;

    // Fallback lighting
    LightingParams fallbackParams_;
};

} // namespace rendering
} // namespace wowee
