#include "core/local_time.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/light_band_block.hpp"
#include "rendering/light_headroom.hpp"
#include "rendering/daylight_fill.hpp"
#include "rendering/light_params_eval.hpp"
#include "rendering/light_volume_order.hpp"
#include <glm/gtc/constants.hpp>
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <cstdlib>

namespace wowee {
namespace rendering {


constexpr uint16_t kHalfMinutesPerDay = LIGHT_HALF_MINUTES_PER_DAY;

// Maximum volumes to blend (top 2-4)
constexpr size_t MAX_BLEND_VOLUMES = 2;

LightingManager::LightingManager() {
    // Set fallback lighting (Elwynn Forest-ish outdoor daytime)
    fallbackParams_.ambientColor = glm::vec3(0.5f, 0.5f, 0.6f);
    fallbackParams_.diffuseColor = glm::vec3(1.0f, 0.95f, 0.85f);
    fallbackParams_.directionalDir = glm::normalize(glm::vec3(0.3f, -0.7f, 0.6f));
    fallbackParams_.fogColor = glm::vec3(0.6f, 0.7f, 0.85f);
    fallbackParams_.fogStart = 300.0f;
    fallbackParams_.fogEnd = 1500.0f;
    fallbackParams_.skyTopColor = glm::vec3(0.4f, 0.6f, 0.9f);
    fallbackParams_.skyMiddleColor = glm::vec3(0.6f, 0.75f, 0.95f);

    currentParams_ = fallbackParams_;
}

LightingManager::~LightingManager() {
}

bool LightingManager::initialize(pipeline::AssetManager* assetManager) {
    if (!assetManager) {
        LOG_ERROR("LightingManager::initialize: null AssetManager");
        return false;
    }


    // Load DBCs (non-fatal if missing, will use fallback lighting)
    loadLightDbc(assetManager);
    loadLightParamsDbc(assetManager);
    loadLightSkyboxDbc(assetManager);
    loadLightBandDbcs(assetManager);

    initialized_ = true;
    LOG_INFO("LightingManager initialized: ", lightVolumesByMap_.size(), " maps with lighting");
    return true;
}

bool LightingManager::loadLightDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\Light.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("Light.dbc not found, using fallback lighting");
        return false;
    }

    auto dbc = std::make_unique<pipeline::DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load Light.dbc");
        return false;
    }

    uint32_t recordCount = dbc->getRecordCount();
    LOG_INFO("Loading Light.dbc: ", recordCount, " light volumes");

    // Parse light volumes
    // Light.dbc structure (WotLK 3.3.5a):
    // 0: uint32 ID
    // 1: uint32 MapID
    // 2-4: float X, Z, Y (note: z and y swapped!)
    // 5: float FalloffStart (inner radius)
    // 6: float FalloffEnd (outer radius)
    // 7: uint32 LightParamsID (clear)
    // 8: uint32 LightParamsID (clear, under water)
    // 9: uint32 LightParamsID (storm)
    // 10: uint32 LightParamsID (storm, under water)
    // 11: uint32 LightParamsID (death), and unused ones after it
    //
    // 8 and 9 were read as rain and under water, so rain drew the under-water
    // light and diving drew the storm. The order is the wowdev DB/Light page's
    // (ParamsClear, ParamsClearWat, ParamsStorm, ParamsStormWat, ParamsDeath;
    // storm covers rain, snow and sandstorm), and WebWowViewerCpp reads slot 1
    // as the under-water fog. The layout names for them changed with
    // the fix, so a dbc_layouts.json copied before it reports the missing
    // fields instead of carrying the old columns on.

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* lL = activeLayout ? activeLayout->getLayout("Light") : nullptr;

    for (uint32_t i = 0; i < recordCount; ++i) {
        LightVolume volume;
        volume.lightId = dbc->getUInt32(i, lL ? (*lL)["ID"] : 0);
        volume.mapId = dbc->getUInt32(i, lL ? (*lL)["MapID"] : 1);

        // Into world space: thirty-sixths of a yard on the tile grid's axes,
        // which are mirrored and swapped against the world's. Checked against
        // six zones whose world coordinates are known - Tirisfal, Undercity,
        // Stormwind, Ironforge, Westfall and Booty Bay - each of which lands
        // inside a volume only under this mapping.
        const float dbcX = dbc->getFloat(i, lL ? (*lL)["X"] : 2);
        const float dbcZ = dbc->getFloat(i, lL ? (*lL)["Z"] : 3);
        const float dbcY = dbc->getFloat(i, lL ? (*lL)["Y"] : 4);
        volume.position = lightPositionToWorld(dbcX, dbcY, dbcZ);

        volume.innerRadius =
            dbc->getFloat(i, lL ? (*lL)["InnerRadius"] : 5) / LIGHT_COORD_UNITS_PER_YARD;
        volume.outerRadius =
            dbc->getFloat(i, lL ? (*lL)["OuterRadius"] : 6) / LIGHT_COORD_UNITS_PER_YARD;

        // LightParams IDs for different conditions
        volume.lightParamsId = dbc->getUInt32(i, lL ? (*lL)["LightParamsID"] : 7);
        volume.lightParamsIdUnderwater =
            dbc->getUInt32(i, lL ? (*lL)["LightParamsIDClearUnderwater"] : 8);
        volume.lightParamsIdStorm = dbc->getUInt32(i, lL ? (*lL)["LightParamsIDStorm"] : 9);
        volume.lightParamsIdStormUnderwater =
            dbc->getUInt32(i, lL ? (*lL)["LightParamsIDStormUnderwater"] : 10);

        // Add to map-specific list
        lightVolumesByMap_[volume.mapId].push_back(volume);
    }

    LOG_INFO("Loaded ", lightVolumesByMap_.size(), " maps with lighting volumes");
    return true;
}

bool LightingManager::loadLightParamsDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\LightParams.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("LightParams.dbc not found");
        return false;
    }

    auto dbc = std::make_unique<pipeline::DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load LightParams.dbc");
        return false;
    }

    uint32_t recordCount = dbc->getRecordCount();
    LOG_INFO("Loaded LightParams.dbc: ", recordCount, " profiles");

    // Create profile entries (will be populated by band loading)
    const auto* lpL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightParams") : nullptr;
    for (uint32_t i = 0; i < recordCount; ++i) {
        uint32_t paramId = dbc->getUInt32(i, lpL ? (*lpL)["LightParamsID"] : 0);
        LightParamsProfile profile;
        profile.lightParamsId = paramId;
        if (dbc->getFieldCount() > 2) {
            profile.lightSkyboxId = dbc->getUInt32(i, 2);
        }
        lightParamsProfiles_[paramId] = profile;
    }

    return true;
}

bool LightingManager::loadLightSkyboxDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\LightSkybox.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("LightSkybox.dbc not found");
        return false;
    }

    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData) || dbc.getFieldCount() < 2) {
        LOG_ERROR("Failed to load LightSkybox.dbc");
        return false;
    }

    // ID, model path, flags. The flags column is WotLK's; a file without it
    // reads as every sky standing in for the client's own.
    const bool hasFlags = dbc.getFieldCount() > 2;
    lightSkyboxes_.clear();
    size_t combining = 0;
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        const uint32_t id = dbc.getUInt32(i, 0);
        std::string path = dbc.getString(i, 1);
        if (id == 0 || path.empty()) continue;
        const uint32_t flags = hasFlags ? dbc.getUInt32(i, 2) : 0;
        if (flags & LIGHT_SKYBOX_COMBINE_PROCEDURAL) ++combining;
        lightSkyboxes_[id] = {.path = std::move(path), .flags = flags};
    }
    LOG_INFO("Loaded LightSkybox.dbc: ", lightSkyboxes_.size(), " model paths, ", combining,
             " drawn over the client's own sky", hasFlags ? "" : " (no flags column)");
    return !lightSkyboxes_.empty();
}

bool LightingManager::loadLightBandDbcs(pipeline::AssetManager* assetManager) {
    // Load LightIntBand.dbc for RGB color curves (18 channels per LightParams)
    auto intBandData = assetManager->readFile("DBFilesClient\\LightIntBand.dbc");
    if (!intBandData.empty()) {
        auto dbc = std::make_unique<pipeline::DBCFile>();
        if (dbc->load(intBandData)) {
            LOG_INFO("Loaded LightIntBand.dbc: ", dbc->getRecordCount(), " color bands");

            // Parse int bands
            // Structure: ID, Entry (block index), NumValues, Time[16], Color[16]
            // Block index = LightParamsID * 18 + channel
            const auto* libL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightIntBand") : nullptr;
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t blockIndex =
                    dbc->getUInt32(i, libL ? (*libL)["BlockIndex"] : 0);
                const LightBandSlot slot =
                    lightBandSlot(blockIndex, LIGHT_INT_CHANNELS);
                if (!slot.valid) continue;

                auto it = lightParamsProfiles_.find(slot.lightParamsId);
                if (it == lightParamsProfiles_.end()) continue;

                const uint32_t channelIndex = slot.channel;
                if (channelIndex >= LightParamsProfile::COLOR_CHANNEL_COUNT) continue;

                ColorBand& band = it->second.colorBands[channelIndex];
                band.numKeyframes = dbc->getUInt32(i, libL ? (*libL)["NumKeyframes"] : 1);
                if (band.numKeyframes > 16) band.numKeyframes = 16;

                // Read time keys (field 3-18) - stored as uint16 half-minutes
                uint32_t timeKeyBase = libL ? (*libL)["TimeKey0"] : 2;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t timeValue = dbc->getUInt32(i, timeKeyBase + k);
                    band.times[k] = static_cast<uint16_t>(timeValue % kHalfMinutesPerDay);  // Clamp to valid range
                }

                // Read color values (field 19-34) - stored as BGRA packed uint32
                uint32_t valueBase = libL ? (*libL)["Value0"] : 18;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t colorBGRA = dbc->getUInt32(i, valueBase + k);
                    band.colors[k] = dbcColorToVec3(colorBGRA);
                }
            }
        }
    }

    // Load LightFloatBand.dbc for fog/intensity curves (6 channels per LightParams)
    auto floatBandData = assetManager->readFile("DBFilesClient\\LightFloatBand.dbc");
    if (!floatBandData.empty()) {
        auto dbc = std::make_unique<pipeline::DBCFile>();
        if (dbc->load(floatBandData)) {
            LOG_INFO("Loaded LightFloatBand.dbc: ", dbc->getRecordCount(), " float bands");

            // Parse float bands
            // Structure: ID, Entry (block index), NumValues, Time[16], Value[16]
            // Block index = LightParamsID * 6 + channel
            const auto* lfbL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightFloatBand") : nullptr;
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t blockIndex =
                    dbc->getUInt32(i, lfbL ? (*lfbL)["BlockIndex"] : 0);
                const LightBandSlot slot =
                    lightBandSlot(blockIndex, LIGHT_FLOAT_CHANNELS);
                if (!slot.valid) continue;

                auto it = lightParamsProfiles_.find(slot.lightParamsId);
                if (it == lightParamsProfiles_.end()) continue;

                const uint32_t channelIndex = slot.channel;
                if (channelIndex >= LightParamsProfile::FLOAT_CHANNEL_COUNT) continue;

                FloatBand& band = it->second.floatBands[channelIndex];
                band.numKeyframes = dbc->getUInt32(i, lfbL ? (*lfbL)["NumKeyframes"] : 1);
                if (band.numKeyframes > 16) band.numKeyframes = 16;

                // Read time keys (field 3-18)
                uint32_t timeKeyBase = lfbL ? (*lfbL)["TimeKey0"] : 2;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t timeValue = dbc->getUInt32(i, timeKeyBase + k);
                    band.times[k] = static_cast<uint16_t>(timeValue % kHalfMinutesPerDay);  // Clamp to valid range
                }

                // Read float values (field 19-34)
                uint32_t valueBase = lfbL ? (*lfbL)["Value0"] : 18;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    band.values[k] = dbc->getFloat(i, valueBase + k);
                }
            }
        }
    }

    LOG_INFO("Loaded bands for ", lightParamsProfiles_.size(), " LightParams profiles");
    return true;
}

void LightingManager::update(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId,
                              float gameTime,
                              bool isRaining, bool isUnderwater) {
    if (!initialized_) return;

    // Update time
    if (!manualTime_) {
        if (gameTime >= 0.0f) {
            // Server-sent game time, in hours since midnight. It was being
            // divided by 86400 as though it were seconds, against a field that
            // held a raw packed bitfield - so the sky's clock was a number with
            // no relation to the hour, and it looked plausible because any
            // value lands somewhere in the day.
            timeOfDay_ = std::fmod(gameTime / 24.0f, 1.0f);  // 0.0-1.0
        } else {
            // Fallback: use real time for day/night cycle
            std::time_t now = std::time(nullptr);
            const std::tm localTime = core::localTime(now);
            float secondsSinceMidnight = localTime.tm_hour * 3600.0f +
                                          localTime.tm_min * 60.0f +
                                          localTime.tm_sec;
            timeOfDay_ = secondsSinceMidnight / 86400.0f;  // 0.0-1.0
        }
    }
    // else: manualTime_ is set, use timeOfDay_ as-is

    // The hour in half-minutes (WoW DBC format: 0-2879). One clock for every
    // zone, as the client has: Duskwood's gloom and Tirisfal's overcast are in
    // their LightParams curves, not in a different hour.
    uint16_t timeHalfMinutes = static_cast<uint16_t>(timeOfDay_ * static_cast<float>(kHalfMinutesPerDay)) % kHalfMinutesPerDay;

    // Update player position and map
    currentPlayerPos_ = playerPos;

    // Find light volumes for blending
    activeVolumes_ = findLightVolumes(playerPos, mapId);

    // Smoothing by real time. This assumed sixty frames a second, so on a
    // display running at a hundred and twenty every blend below ran twice as
    // fast, and at thirty half as fast.
    const auto now = std::chrono::steady_clock::now();
    float deltaTime = 0.016f;
    if (lastUpdate_.time_since_epoch().count() != 0) {
        deltaTime = std::clamp(std::chrono::duration<float>(now - lastUpdate_).count(),
                               0.0f, 0.25f);
    }
    lastUpdate_ = now;
    const float blendFactor = 1.0f - std::exp(-deltaTime * 5.0f);

    // Which sky models are overhead, and how much of each.
    //
    // One model was chosen and held until the player left its zone's
    // falloff, then swapped for the next in a single frame - while the sky
    // colours had been blending across that same falloff all along. So at a
    // border the old zone's sky sat over colours already most of the way to
    // the new one: Hellfire's nebula stops at a line of longitude, and past it
    // the sky behind was already Terokkar's pale blue, a hard edge down the
    // middle of the screen. Then it popped.
    //
    // Now each model is up by the weight of the lights that name it, over the
    // same volumes and with the same smoothing the colours use, so the two
    // cross the border together. A light with no sky model contributes to no
    // layer, and the models fade toward the plain sky there.
    //
    // Gathered by LightSkybox row, as WoW.exe gathers its slots (0x7ed713):
    // lights naming the same row add their weights, and two rows naming one
    // model are two layers. Several rows share a model with different flags -
    // DeathKnightFireSkyBox is 38 and 39 without the combine flag and 40 with
    // it - and whether the client's sky is drawn under it is the row's say.
    {
        std::vector<std::pair<uint32_t, float>> target;
        if (!isIndoors_) {
            const size_t blendCount = std::min(activeVolumes_.size(), MAX_BLEND_VOLUMES);
            for (size_t i = 0; i < blendCount; ++i) {
                const auto& wv = activeVolumes_[i];
                const uint32_t paramsId = selectLightParamsId(*wv.volume, isRaining, isUnderwater);
                auto profileIt = lightParamsProfiles_.find(paramsId);
                if (profileIt == lightParamsProfiles_.end() ||
                    profileIt->second.lightSkyboxId == 0) continue;
                const uint32_t skyboxId = profileIt->second.lightSkyboxId;
                if (!lightSkyboxes_.contains(skyboxId)) {
                    if (missingSkyboxIds_.insert(skyboxId).second) {
                        LOG_WARNING("LightParams ", paramsId, " names LightSkybox ", skyboxId,
                                    ", which LightSkybox.dbc does not have: no sky model there");
                    }
                    continue;
                }
                auto t = std::find_if(target.begin(), target.end(),
                                      [&](const auto& e) { return e.first == skyboxId; });
                if (t == target.end()) target.emplace_back(skyboxId, wv.weight);
                else t->second = std::min(1.0f, t->second + wv.weight);
            }
        }
        auto targetFor = [&](uint32_t id) {
            for (const auto& [i, w] : target) if (i == id) return w;
            return 0.0f;
        };
        for (auto& layer : skyboxLayers_) {
            layer.weight += (targetFor(layer.id) - layer.weight) * blendFactor;
        }
        for (const auto& [id, w] : target) {
            const bool present = std::any_of(skyboxLayers_.begin(), skyboxLayers_.end(),
                                             [&](const SkyboxLayer& l) { return l.id == id; });
            if (present) continue;
            const SkyboxRow& row = lightSkyboxes_.at(id);
            skyboxLayers_.push_back({.id = id, .path = row.path, .flags = row.flags,
                                     .weight = w * blendFactor});
        }
        // Gone once faded out and not wanted - never while it is still wanted,
        // so a model is not dropped and reloaded on its way in.
        std::erase_if(skyboxLayers_, [&](const SkyboxLayer& l) {
            return l.weight < 0.005f && targetFor(l.id) <= 0.0f;
        });
        std::sort(skyboxLayers_.begin(), skyboxLayers_.end(),
                  [](const SkyboxLayer& a, const SkyboxLayer& b) { return a.weight > b.weight; });
    }

    // Sample and blend lighting. Indoors too: this is the outdoor light, which
    // lights what is seen through a door; models inside an interior group take
    // their own light in the shaders.
    LightingParams newParams;

    std::optional<LightingParams> blended;
    {
        // The first MAX_BLEND_VOLUMES only: the list is the whole
        // neighbourhood now, and those are the ones whose weights were
        // normalized to sum to one.
        std::array<WeightedLightingParams, MAX_BLEND_VOLUMES> parts;
        size_t partCount = 0;
        const size_t blendCount = std::min(activeVolumes_.size(), MAX_BLEND_VOLUMES);
        for (size_t i = 0; i < blendCount; ++i) {
            const auto& wv = activeVolumes_[i];
            const uint32_t lightParamsId = selectLightParamsId(*wv.volume, isRaining, isUnderwater);
            auto it = lightParamsProfiles_.find(lightParamsId);
            if (it == lightParamsProfiles_.end()) continue;
            parts[partCount++] = {
                .params = sampleLightParams(it->second, timeHalfMinutes, fallbackParams_),
                .weight = wv.weight};

            // Debug logging for fog params (first few samples only)
            static int debugCount = 0;
            if (debugCount < 3) {
                const LightingParams& p = parts[partCount - 1].params;
                LOG_INFO("Fog params: start=", p.fogStart, " end=", p.fogEnd,
                         " color=(", p.fogColor.r, ",", p.fogColor.g, ",", p.fogColor.b, ")");
                debugCount++;
            }
        }
        blended = blendLightingParams(std::span(parts.data(), partCount));
    }

    if (blended) {
        newParams = *blended;
    } else {
        // No light volume, or none naming a LightParams row: the fallback,
        // with time-based animation
        newParams = fallbackParams_;
        newParams.directionalDir = sunLightDirection(timeOfDay_);

        // Time-of-day color adjustments
        if (timeOfDay_ < 0.25f || timeOfDay_ > 0.75f) {
            // Night: darker, bluer
            float nightness = (timeOfDay_ < 0.25f) ? (0.25f - timeOfDay_) * 4.0f
                                                    : (timeOfDay_ - 0.75f) * 4.0f;
            newParams.ambientColor *= (0.3f + 0.7f * (1.0f - nightness));
            newParams.diffuseColor *= (0.2f + 0.8f * (1.0f - nightness));
            newParams.ambientColor.b += nightness * 0.1f;
        }
    }

    // Keep the authored fog colour. The surface shaders blend it toward the
    // actual sky band along each camera ray; one CPU-side mid-sky tint made
    // buildings below the horizon stand out as pale silhouettes.

    // How much fog, as a multiplier on the distances the zone asks for.
    //
    // Fog is drawn between fogStart and fogEnd, so bringing both in thickens
    // it and pushing both out clears it. Scaling by 1/strength keeps the
    // shape of the zone's own curve and only moves where it sits: 1.0 is the
    // DBC exactly, 2.0 puts the same gradient at half the distance, and 0.5
    // doubles it.
    //
    // Zero is off rather than infinitely thick. A strength of zero scaling
    // distances by 1/0 is where a divide would land, and what the player means
    // by nothing is no fog at all.
    const float strength = glm::clamp(fogStrength_, 0.0f, 2.0f);
    if (strength <= 0.001f) {
        newParams.fogStart = 1.0e6f;
        newParams.fogEnd = 1.0e6f + 1.0f;
    } else if (std::abs(strength - 1.0f) > 0.001f) {
        const float scale = 1.0f / strength;
        newParams.fogStart *= scale;
        newParams.fogEnd *= scale;
    }

    // Every lit shader adds these as `ambient + diffuse * N-dot-L`, and on
    // ground facing the sun that is nearly their plain sum. 82% of the
    // client's 844 LightParams rows exceed 1.0 in some channel at noon, the
    // worst reaching 2.00, and clipping happens per channel - the ones that
    // overflow stop while the rest keep their value, and the hue slides toward
    // whatever saturated. Scaling both together keeps the hue and only ever
    // darkens.
    // The offline comparison runs both treatments with identical world data.
    const bool baselineProbe = std::getenv("WOWEE_ORGRIMMAR_PROBE") &&
                               std::getenv("WOWEE_ORGRIMMAR_PROBE_BASELINE");
    if (!isIndoors_ && !baselineProbe) {
        applyDaylightFill(newParams.ambientColor, newParams.diffuseColor);
    }
    applyLightHeadroom(newParams.ambientColor, newParams.diffuseColor);

    // What the sky is being told, whenever it changes.
    //
    // Three separate boundary faults have been fixed behind a report of the
    // sky changing brightness as the player walks, and it is still reported.
    // Guessing at a fourth is worth less than one line saying which input
    // moved: the visual hour, the zone, the sky model, the volumes being
    // blended, or none of them - which would put it past this file entirely.
    //
    // Only on a change, so standing still is silent and a walk prints one line
    // per event rather than one per frame. INFO, so it costs nothing until
    // somebody asks for it with WOWEE_LOG_LEVEL=info.
    {
        const float skyLuma = 0.2126f * newParams.skyTopColor.r +
                              0.7152f * newParams.skyTopColor.g +
                              0.0722f * newParams.skyTopColor.b;
        const std::string topSkybox = skyboxLayers_.empty() ? std::string() : skyboxLayers_.front().path;
        uint32_t firstVolume = 0, secondVolume = 0;
        if (!activeVolumes_.empty()) firstVolume = activeVolumes_[0].volume->lightId;
        if (activeVolumes_.size() > 1) secondVolume = activeVolumes_[1].volume->lightId;
        const bool changed =
            zoneId != diagZoneId_ || topSkybox != diagSkyboxPath_ ||
            firstVolume != diagFirstVolume_ || secondVolume != diagSecondVolume_ ||
            std::abs(getTimeOfDayHours() - diagHours_) > 0.02f ||
            std::abs(skyLuma - diagSkyLuma_) > 0.01f;
        // Rate-limited as well as change-gated, because a change is not rare:
        // walking through the falloff between two volumes moves the target
        // luminance every frame, and a formatted write per frame is what this
        // whole investigation turned out to be about. Half a second is fast
        // enough to see what is oscillating and slow enough to cost nothing.
        ++diagCallsSinceLog_;
        if (changed && diagCallsSinceLog_ >= 30) {
            diagCallsSinceLog_ = 0;
            LOG_INFO("sky: zone=", zoneId, " hour=", getTimeOfDayHours(),
                     " skyLuma=", skyLuma, " volumes=", firstVolume, "/", secondVolume,
                     " inRange=", activeVolumes_.size(),
                     " skybox=", topSkybox.empty() ? "-" : topSkybox);
            diagZoneId_ = zoneId;
            diagSkyboxPath_ = topSkybox;
            diagFirstVolume_ = firstVolume;
            diagSecondVolume_ = secondVolume;
            diagHours_ = getTimeOfDayHours();
            diagSkyLuma_ = skyLuma;
        }
    }

    // Smooth temporal blending to avoid snapping (5.0 = blend rate); see
    // blendFactor above.

    currentParams_.ambientColor = glm::mix(currentParams_.ambientColor, newParams.ambientColor, blendFactor);
    currentParams_.diffuseColor = glm::mix(currentParams_.diffuseColor, newParams.diffuseColor, blendFactor);
    currentParams_.fogColor = glm::mix(currentParams_.fogColor, newParams.fogColor, blendFactor);
    currentParams_.skyTopColor = glm::mix(currentParams_.skyTopColor, newParams.skyTopColor, blendFactor);
    currentParams_.skyMiddleColor = glm::mix(currentParams_.skyMiddleColor, newParams.skyMiddleColor, blendFactor);
    currentParams_.skyBand1Color = glm::mix(currentParams_.skyBand1Color, newParams.skyBand1Color, blendFactor);
    currentParams_.skyBand2Color = glm::mix(currentParams_.skyBand2Color, newParams.skyBand2Color, blendFactor);
    currentParams_.sunColor = glm::mix(currentParams_.sunColor, newParams.sunColor, blendFactor);
    currentParams_.sunHaloColor = glm::mix(currentParams_.sunHaloColor, newParams.sunHaloColor, blendFactor);
    currentParams_.fogStart = glm::mix(currentParams_.fogStart, newParams.fogStart, blendFactor);
    currentParams_.fogEnd = glm::mix(currentParams_.fogEnd, newParams.fogEnd, blendFactor);
    currentParams_.fogDensity = glm::mix(currentParams_.fogDensity, newParams.fogDensity, blendFactor);
    currentParams_.cloudDensity = glm::mix(currentParams_.cloudDensity, newParams.cloudDensity, blendFactor);
    currentParams_.horizonGlow = glm::mix(currentParams_.horizonGlow, newParams.horizonGlow, blendFactor);
    currentParams_.directionalDir = glm::normalize(glm::mix(currentParams_.directionalDir, newParams.directionalDir, blendFactor));
}

bool LightingManager::skyModelsHideProceduralSky(
    const std::function<bool(uint32_t)>& isLoaded) const {
    return std::any_of(skyboxLayers_.begin(), skyboxLayers_.end(), [&](const SkyboxLayer& l) {
        return l.hidesProceduralSky() && isLoaded(l.id);
    });
}

std::vector<LightingManager::WeightedVolume> LightingManager::findLightVolumes(const glm::vec3& playerPos, uint32_t mapId) const {
    auto it = lightVolumesByMap_.find(mapId);
    if (it == lightVolumesByMap_.end()) {
        return {};
    }

    const std::vector<LightVolume>& volumes = it->second;
    if (volumes.empty()) {
        return {};
    }

    // Collect all volumes with weight > 0
    std::vector<WeightedVolume> weighted;
    weighted.reserve(volumes.size());

    for (const auto& volume : volumes) {
        glm::vec3 toPlayer = playerPos - volume.position;
        float distSq = glm::dot(toPlayer, toPlayer);

        float weight = 0.0f;
        if (distSq <= volume.innerRadius * volume.innerRadius) {
            // Inside inner radius: full weight
            weight = 1.0f;
        } else if (distSq < volume.outerRadius * volume.outerRadius) {
            // Between inner and outer: fade out with smoothstep (sqrt needed for interpolation)
            float dist = std::sqrt(distSq);
            float t = (dist - volume.innerRadius) / (volume.outerRadius - volume.innerRadius);
            t = glm::clamp(t, 0.0f, 1.0f);
            weight = 1.0f - (t * t * (3.0f - 2.0f * t));  // Smoothstep
        }

        if (weight > 0.0f) {
            weighted.push_back({.volume = &volume, .weight = weight});
        }
    }

    if (weighted.empty()) {
        return {};
    }

    // Keep the top N by weight, ordered by something that cannot change under
    // the player's feet - see light_volume_order.hpp for why weight alone is
    // not an order here and what the flicker looked like.
    const auto moreSpecific = [](const WeightedVolume& a, const WeightedVolume& b) {
        return lightVolumeOrderedBefore(a.weight, a.volume->outerRadius, a.volume->lightId,
                                        b.weight, b.volume->outerRadius, b.volume->lightId);
    };
    // Sorted in full rather than truncated here. Only the first
    // MAX_BLEND_VOLUMES are blended, but the skybox is chosen over the whole
    // list: a volume that names a sky and sits third on weight still says
    // which sky is overhead, and cutting the list here meant it dropped out
    // and back in as the player walked.
    std::sort(weighted.begin(), weighted.end(), moreSpecific);

    // Normalize the ones that will be blended, so their weights sum to 1.0.
    // The rest keep the weight they were given; nothing reads it but the
    // skybox walk, which only asks whether they are in range at all.
    const size_t blended = std::min(weighted.size(), MAX_BLEND_VOLUMES);
    float totalWeight = 0.0f;
    for (size_t i = 0; i < blended; ++i) {
        totalWeight += weighted[i].weight;
    }
    if (totalWeight > 0.0f) {
        for (size_t i = 0; i < blended; ++i) {
            weighted[i].weight /= totalWeight;
        }
    }

    // Which volumes this map turned out to have around the player, once.
    //
    // This was three lines inside the loop above, meant to fire on one frame
    // and guarded by a counter that only decremented when the third volume was
    // pushed. Almost nowhere has three: Hellfire Peninsula has one. So the
    // counter never reached zero and the line was written every frame, for the
    // whole session - 534 of one log's 3384 lines, still going four minutes in.
    //
    // It reads as movement-triggered, which is what makes it worth explaining
    // rather than just deleting. The logger collapses a message identical to
    // the one before it, so standing still folds into "previous message
    // repeated" and walking changes distSq every frame and folds into nothing.
    // Whatever a per-frame formatted write costs, it was being paid only while
    // the player moved, and only with WOWEE_LOG_LEVEL=info.
    if (mapId != diagLoggedMapId_) {
        diagLoggedMapId_ = mapId;
        std::string named;
        for (size_t i = 0; i < weighted.size() && i < 3; ++i) {
            const LightVolume& v = *weighted[i].volume;
            named += " [" + std::to_string(v.lightId) + " inner=" +
                     std::to_string(static_cast<int>(v.innerRadius)) + " outer=" +
                     std::to_string(static_cast<int>(v.outerRadius)) + "]";
        }
        LOG_INFO("Light volumes on map ", mapId, ": ", weighted.size(),
                 " in range,", named.empty() ? " none" : named);
    }

    return weighted;
}

glm::vec3 LightingManager::dbcColorToVec3(uint32_t dbcColor) const {
    // Red is the high byte: the packed value is 0x00RRGGBB. Reading it the
    // other way round swaps red and blue, which turns Teldrassil's violet
    // canopies magenta and leaves every zone's light fighting its own sky.
    //
    // Checked against the file rather than the layout's name for it. Over the
    // 844 LightParams rows that carry a first channel, taking red from the
    // high byte makes 66% of them warm at noon and 64% blue at midnight;
    // taking it from the low byte gives 35% and 41%, which is worse than
    // chance in both directions - the mark of a colour read backwards.
    const uint8_t r = (dbcColor >> 16) & 0xFF;
    const uint8_t g = (dbcColor >> 8) & 0xFF;
    const uint8_t b = dbcColor & 0xFF;

    return glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
}

} // namespace rendering
} // namespace wowee
