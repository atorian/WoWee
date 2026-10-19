#include "core/world_loader.hpp"
#include "collision/collision_world.hpp"
#include "core/frame_phase.hpp"
#include "core/thread_pool.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/camera.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/texture_upload.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wdt_loader.hpp"
#include "pipeline/dbc_loader.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <glm/gtc/quaternion.hpp>
#include <unordered_set>
#include <utility>

namespace wowee::core {
namespace {
using TimePoint = std::chrono::steady_clock::time_point;
std::string textureKey(std::string path) {
    if (auto nul = path.find('\0'); nul != std::string::npos) path.resize(nul);
    std::replace(path.begin(), path.end(), '/', '\\');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
    if (path.ends_with(".tga") || path.ends_with(".dds")) path.replace(path.size() - 4, 4, ".blp");
    return path;
}
}

struct WorldLoader::PreparedMap {
    pipeline::WDTInfo wdt;
    pipeline::WMOModel root;
    glm::vec3 position{0}, rotation{0};
    struct Doodad {
        uint32_t modelId;
        std::shared_ptr<pipeline::M2Model> model;
        glm::mat4 matrix;
        glm::vec3 position;
        std::optional<rendering::InteriorLight> interior;
    };
    std::vector<Doodad> doodads;
    std::unordered_map<std::string, pipeline::BLPImage> wmoTextures, m2Textures;
    std::unordered_map<std::string, rendering::EncodedNormalMap> normals;
    std::string error;
    bool instance() const { return wdt.isWMOOnly() && !wdt.rootWMOPath.empty(); }
};

struct WorldLoader::Load {
    Entry entry;
    std::string mapName;
    glm::vec3 spawn;
    Stage stage = Stage::Preparing;
    WorldLoadStatus status;
    JobGroup::Token token;
    JobGroup::Future<std::shared_ptr<PreparedMap>> future;
    std::shared_ptr<PreparedMap> prepared;
    TimePoint started{}, stageStarted{};
    WorldTileProgress tiles;
    WorldWarmupProgress warmup;
    bool rootPlaced = false;
    size_t doodad = 0;
};

WorldLoader::WorldLoader(const Phase& phase, rendering::Renderer* renderer,
                         pipeline::AssetManager* assets, std::filesystem::path lastWorldFile,
                         Clock clock)
    : phase_(phase), renderer_(renderer), assetManager_(assets),
      lastWorldFile_(std::move(lastWorldFile)), clock_(std::move(clock)) {}

WorldLoader::~WorldLoader() { cancelWorldPreload(); }

float WorldLoader::secondsSince(TimePoint start) const {
    return std::chrono::duration<float>(clock_() - start).count();
}

WorldLoader::Session::Session(WorldLoader& owner, std::unique_ptr<Load> load)
    : owner_(owner), load_(std::move(load)) {
    if (owner_.load_) throw std::logic_error("World loader already has a map owner");
    owner_.load_ = load_.get();
}
WorldLoader::Session::~Session() {
    load_->token.cancel();
    if (load_->future.valid()) load_->future.wait();
    owner_.load_ = nullptr;
}
uint32_t WorldLoader::getLoadedMapId() const {
    return load_ && load_->stage == Stage::Finished ? load_->entry.mapId : 0xFFFFFFFF;
}
std::optional<WorldLoader::Entry> WorldLoader::takePendingEntry() {
    return std::exchange(pendingWorldEntry_, std::nullopt);
}

const WorldLoadStatus& WorldLoader::status() const {
    static const WorldLoadStatus idle{WorldLoadResult::Pending, "No world load", 0};
    return load_ ? load_->status : idle;
}
WorldLoader::Stage WorldLoader::stage() const { return load_ ? load_->stage : Stage::Idle; }
WorldLoader::Entry WorldLoader::entry() const { return load_->entry; }
const std::string& WorldLoader::mapName() const { return load_->mapName; }
bool WorldLoader::isInstance() const { return load_ && load_->prepared && load_->prepared->instance(); }
float WorldLoader::warmupSeconds() const {
    return stage() == Stage::Warmup || stage() == Stage::Finished ? secondsSince(load_->stageStarted) : 0;
}

void WorldLoader::fail(const std::string& reason) {
    if (!load_ || load_->stage == Stage::Failed) return;
    load_->stage = Stage::Failed;
    load_->status = {WorldLoadResult::Failed, reason, load_->status.progress};
    LOG_ERROR("World load failed: map=", load_->entry.mapId, " ", reason, "; returning to login");
}

std::string WorldLoader::resolveMapName(uint32_t mapId) {
    if (!mapNameCacheLoaded_) {
        mapNameCacheLoaded_ = true;
        if (auto dbc = assetManager_ ? assetManager_->loadDBC("Map.dbc") : nullptr; dbc && dbc->isLoaded()) {
            const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Map") : nullptr;
            for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
                auto name = dbc->getString(row, layout ? (*layout)["InternalName"] : 1);
                if (!name.empty()) mapNameById_.try_emplace(dbc->getUInt32(row, layout ? (*layout)["ID"] : 0), std::move(name));
            }
        } else LOG_WARNING("Map.dbc not available; using fallback map-id mapping");
    }
    if (auto it = mapNameById_.find(mapId); it != mapNameById_.end()) return it->second;
    return mapIdToName(mapId);
}

std::unique_ptr<WorldLoader::Session> WorldLoader::begin(Entry entry, JobGroup& jobs) {
    if (!requirePhase(phase_, Phase::Transitions, "begin world load")) return {};
    auto session = std::make_unique<Session>(*this, std::make_unique<Load>());
    load_->token = jobs.token();
    load_->entry = entry;
    load_->spawn = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(entry.x, entry.y, entry.z)));
    load_->started = load_->stageStarted = clock_();
    // A scene that is drawn is built from the game data; without it there is
    // nothing to draw the map with.
    if (renderer_ && (!assetManager_ || !assetManager_->isInitialized())) {
        fail("Game data is unavailable, so the map cannot be built");
        return session;
    }
    load_->mapName = resolveMapName(entry.mapId);
    if (load_->mapName.empty()) { fail("No map name for id " + std::to_string(entry.mapId)); return session; }
    cancelWorldPreload();
    saveLastWorldInfo(entry.mapId, load_->mapName, entry.x, entry.y);
    LOG_INFO("World load started: map=", entry.mapId, " name=", load_->mapName,
             " server=(", entry.x, ",", entry.y, ",", entry.z, ")",
             renderer_ ? "" : " (nothing is drawn: no scene to build)");
    if (!renderer_) {
        // Nothing to read for a scene nobody draws: the preparation is empty,
        // and still goes through the same job and the same stages.
        load_->future = jobs.submit([](const JobGroup::Token&) { return std::make_shared<PreparedMap>(); });
        return session;
    }
    auto* assets = assetManager_;
    const auto normalMaps = renderer_->getVkContext()
        ? rendering::normalMapFormatFor(*renderer_->getVkContext())
        : rendering::NormalMapFormat::Rgba8;
    load_->future = jobs.submit([assets, name = load_->mapName, normalMaps](const JobGroup::Token& token) {
        return prepareMap(*assets, name, normalMaps, token);
    });
    return session;
}

std::shared_ptr<WorldLoader::PreparedMap> WorldLoader::prepareMap(pipeline::AssetManager& assets,
        const std::string& mapName, rendering::NormalMapFormat normalMaps, const JobGroup::Token& token) {
    auto result = std::make_shared<PreparedMap>();
    const std::string wdtPath = "World\\Maps\\" + mapName + "\\" + mapName + ".wdt";
    if (!token.current()) return result;
    auto data = assets.readFile(wdtPath);
    if (data.empty()) {
        LOG_WARNING("No WDT at ", wdtPath, "; trying ordinary ADT streaming");
        return result;
    }
    result->wdt = pipeline::parseWDT(data);
    if (!result->instance()) return result;
    auto& model = result->root;
    const auto& path = result->wdt.rootWMOPath;
    data = assets.readFile(path);
    if (!token.current()) return result;
    if (data.empty()) { result->error = "Cannot read root WMO " + path; return result; }
    model = pipeline::WMOLoader::load(data);
    for (uint32_t group = 0; group < model.nGroups; ++group) {
        if (!token.current()) return result;
        bool found = false;
        for (const auto& candidate : pipeline::wmoGroupCandidates(path, group)) {
            data = assets.readFileOptional(candidate);
            if (data.empty()) continue;
            if (!pipeline::WMOLoader::loadGroup(data, model, group)) {
                result->error = "Cannot parse WMO group " + candidate;
                return result;
            }
            found = true;
            break;
        }
        if (!found) { result->error = "Missing WMO group " + std::to_string(group) + " of " + path; return result; }
    }
    if (!model.isValid()) { result->error = "Invalid root WMO " + path; return result; }
    for (const auto& texture : model.textures) {
        if (!token.current()) return result;
        const auto key = textureKey(texture);
        if (key.empty() || result->wmoTextures.contains(key)) continue;
        auto image = assets.loadTexture(key, true);
        if (image.isValid()) {
            const auto pixels = image.isBlockCompressed() ? pipeline::BLPLoader::decodeBaseLevel(image) : image.data;
            auto normal = pixels.empty() ? rendering::EncodedNormalMap{} :
                rendering::WMORenderer::prepareNormalHeightMap(pixels.data(), image.width, image.height, normalMaps);
            if (normal.valid()) result->normals[key] = std::move(normal);
            image.analyzeAlpha();
        } else LOG_WARNING("Instance texture unavailable: ", key, "; renderer will use its fallback");
        result->wmoTextures[key] = std::move(image);
    }
    const auto& wdt = result->wdt;
    result->rotation = glm::vec3(-wdt.rotation[2], -wdt.rotation[0], wdt.rotation[1] + 180.0f) * coords::PI / 180.0f;
    if (wdt.position[0] != 0 || wdt.position[1] != 0 || wdt.position[2] != 0)
        result->position = coords::adtToWorld(wdt.position[0], wdt.position[1], wdt.position[2]);
    glm::mat4 matrix = glm::translate(glm::mat4(1), result->position);
    matrix = glm::rotate(matrix, result->rotation.z, glm::vec3(0, 0, 1));
    matrix = glm::rotate(matrix, result->rotation.y, glm::vec3(0, 1, 0));
    matrix = glm::rotate(matrix, result->rotation.x, glm::vec3(1, 0, 0));
    if (model.doodadSets.empty()) return result;
    const auto& set = model.doodadSets[std::min<size_t>(wdt.doodadSet, model.doodadSets.size() - 1)];
    const auto interiors = pipeline::interiorDoodads(model);
    std::unordered_map<std::string, std::shared_ptr<pipeline::M2Model>> models;
    for (uint32_t index = set.startIndex; index < set.startIndex + set.count && index < model.doodads.size(); ++index) {
        if (!token.current()) return result;
        const auto& doodad = model.doodads[index];
        const auto name = model.doodadNames.find(doodad.nameIndex);
        if (name == model.doodadNames.end() || name->second.empty()) {
            LOG_WARNING("Instance doodad ", index, " in ", path, " has no model path; doodad omitted");
            continue;
        }
        const auto m2Path = pipeline::modelPathToM2(name->second);
        auto [it, inserted] = models.try_emplace(m2Path);
        if (inserted) {
            data = assets.readFile(m2Path);
            auto m2 = std::make_shared<pipeline::M2Model>(pipeline::M2Loader::load(data));
            m2->name = m2Path;
            const auto skin = assets.readFileOptional(pipeline::skinPathForM2(m2Path));
            if (!skin.empty() && m2->version >= 264) pipeline::M2Loader::loadSkin(skin, *m2);
            if (m2->isValid()) {
                it->second = m2;
                for (const auto& texture : m2->textures) {
                    if (!token.current()) return result;
                    const auto key = textureKey(texture.filename);
                    if (key.empty() || result->m2Textures.contains(key)) continue;
                    auto image = assets.loadTexture(key, true);
                    if (image.isValid()) image.analyzeAlpha();
                    result->m2Textures[key] = std::move(image);
                }
            } else LOG_WARNING("Instance doodad model invalid or missing: ", m2Path, "; doodad omitted");
        }
        if (!it->second) continue;
        auto local = glm::translate(glm::mat4(1), doodad.position) * glm::mat4_cast(doodad.rotation);
        local = glm::scale(local, glm::vec3(doodad.scale));
        const auto world = matrix * local;
        std::optional<rendering::InteriorLight> light;
        if (interiors[index]) light = rendering::doodadInteriorLight(model.ambientColor, glm::vec3(doodad.color));
        result->doodads.push_back({static_cast<uint32_t>(std::hash<std::string>{}(m2Path)),
                                  it->second, world, glm::vec3(world[3]), light});
    }
    return result;
}

bool WorldLoader::initializeScene() {
    if (!requirePhase(phase_, Phase::Transitions, "initialize loading scene") || stage() != Stage::Prepared) return false;
    if (!renderer_) {
        load_->stage = Stage::Geometry;
        load_->stageStarted = clock_();
        load_->status = {WorldLoadResult::Pending, "No geometry to build: nothing is drawn", 0.35f};
        return true;
    }
    try {
        const auto& name = load_->mapName;
        bool ok = false;
        if (isInstance()) {
            ok = renderer_->initializeRenderers(assetManager_, name);
        } else {
            const auto canonical = coords::serverToCanonical(glm::vec3(load_->entry.x, load_->entry.y, load_->entry.z));
            const auto [x, y] = coords::canonicalToTile(canonical.x, canonical.y);
            ok = renderer_->loadTestTerrain(assetManager_, "World\\Maps\\" + name + "\\" + name + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt");
        }
        if (!ok) { fail("Cannot initialize geometry for " + name); return false; }
        if (auto* terrain = renderer_->getTerrainManager()) {
            terrain->setStreamingEnabled(!isInstance());
            if (!isInstance()) {
                terrain->setLoadRadius(4);
                terrain->setUnloadRadius(9);
                terrain->update(*renderer_->getCamera(), 1.0f);
            }
        }
        load_->stage = Stage::Geometry;
        load_->stageStarted = clock_();
        load_->status = {WorldLoadResult::Pending, isInstance() ? "Uploading instance geometry..." : "Streaming terrain tiles...", 0.35f};
        if (!isInstance()) {
            auto* terrain = renderer_->getTerrainManager();
            (void)load_->tiles.observe(0, terrain->getLoadedTileCount(), terrain->getRemainingTileCount());
        }
        return true;
    } catch (const std::exception& error) {
        fail(error.what());
        return false;
    }
}

const WorldLoadStatus& WorldLoader::update(float /*deltaTime*/) {
    if (!requirePhase(phase_, Phase::Streaming, "stream world load")) return status();
    if (!load_ || stage() == Stage::Finished || stage() == Stage::Failed) return status();
    auto& load = *load_;
    auto* ctx = renderer_ ? renderer_->getVkContext() : nullptr;
    if (ctx && ctx->isDeviceLost()) { fail("GPU device lost"); return status(); }
    try {
        if (stage() == Stage::Preparing) {
            if (load.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return status();
            load.prepared = load.future.get();
            if (!load.prepared->error.empty()) { fail(load.prepared->error); return status(); }
            load.stage = Stage::Prepared;
            load.status = {WorldLoadResult::Pending, "Preparing world renderers...", 0.2f};
        } else if (stage() == Stage::Geometry) {
            bool done = false;
            if (!renderer_) {
                // A scene nobody draws has no geometry to wait for.
                done = true;
            } else if (isInstance()) {
                auto& prepared = *load.prepared;
                auto* wmo = renderer_->getWMORenderer();
                auto* m2 = renderer_->getM2Renderer();
                if (!wmo || !m2) { fail("Instance renderers unavailable"); return status(); }
                struct UploadScope {
                    rendering::VkContext* ctx;
                    rendering::WMORenderer* wmo;
                    rendering::M2Renderer* m2;
                    UploadScope(rendering::VkContext* c, rendering::WMORenderer* w, rendering::M2Renderer* m, PreparedMap& p)
                        : ctx(c), wmo(w), m2(m) {
                        if (ctx) ctx->beginUploadBatch();
                        wmo->setPredecodedBLPCache(&p.wmoTextures);
                        wmo->setPredecodedNormalMapCache(&p.normals);
                        wmo->setDeferNormalMaps(true);
                        m2->setPredecodedBLPCache(&p.m2Textures);
                    }
                    ~UploadScope() {
                        m2->setPredecodedBLPCache(nullptr);
                        wmo->setPredecodedBLPCache(nullptr);
                        wmo->setPredecodedNormalMapCache(nullptr);
                        wmo->setDeferNormalMaps(false);
                        if (ctx) ctx->endUploadBatch();
                    }
                } uploads(ctx, wmo, m2, prepared);
                // A budget of this frame's work, so the real clock rather than
                // the one the stages are timed by.
                const auto started = std::chrono::steady_clock::now();
                const auto uploadSeconds = [&started] {
                    return std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
                };
                if (!load.rootPlaced) {
                    const uint32_t modelId = 900000 + load.entry.mapId;
                    const auto result = wmo->loadModelIncremental(prepared.root, modelId, 2.0f);
                    if (result == rendering::WMORenderer::ModelLoadResult::Failed) {
                        fail("Root WMO upload failed: " + prepared.wdt.rootWMOPath);
                        return status();
                    }
                    if (result == rendering::WMORenderer::ModelLoadResult::InProgress) return status();
                    if (!renderer_->scene().worldModels().createInstance(modelId, prepared.position,
                                                                         prepared.rotation, 1.0f)) {
                        fail("Root WMO instance creation failed"); return status();
                    }
                    load.rootPlaced = true;
                }
                while (load.doodad < prepared.doodads.size() && uploadSeconds() < 0.002f) {
                    const auto& d = prepared.doodads[load.doodad++];
                    if (!m2->loadModel(*d.model, d.modelId)) {
                        LOG_WARNING("Instance doodad upload failed: ", d.model->name, "; doodad omitted");
                        continue;
                    }
                    auto& models = renderer_->scene().models();
                    const auto id = models.createInstanceWithMatrix(d.modelId, d.matrix, d.position);
                    if (id) { models.setSkipWallCollision(id, true); if (d.interior) models.setInteriorLight(id, d.interior); }
                    else LOG_WARNING("Instance doodad instance creation failed: ", d.model->name, "; doodad omitted");
                }
                done = load.doodad == prepared.doodads.size();
                load.status.progress = 0.35f + 0.5f * static_cast<float>(load.doodad) / std::max<size_t>(1, prepared.doodads.size());
            } else if (auto* terrain = renderer_->getTerrainManager()) {
                terrain->processOneReadyTile();
                load.status = load.tiles.observe(secondsSince(load.stageStarted), terrain->getLoadedTileCount(), terrain->getRemainingTileCount());
                done = load.status.result == WorldLoadResult::Ready;
                if (done) {
                    if (terrain->getRemainingTileCount() > 0) LOG_WARNING(load.status.reason, " (map=", load.entry.mapId, ")");
                    terrain->setLoadRadius(renderer_->getTerrainLoadRadius());
                    terrain->setUnloadRadius(renderer_->getTerrainUnloadRadius());
                }
            } else { fail("Terrain manager unavailable"); return status(); }
            if (done) {
                load.stage = Stage::GeometryReady;
                load.status = {WorldLoadResult::Pending, "Preparing creatures...", 0.97f};
            }
        }
    } catch (const std::exception& error) { fail(error.what()); }
    return status();
}

void WorldLoader::observeReadiness(bool spawnsPending) {
    if (!requirePhase(phase_, Phase::Present, "observe world readiness") || stage() != Stage::Warmup) return;
    auto& load = *load_;
    auto* ctx = renderer_ ? renderer_->getVkContext() : nullptr;
    // Nothing to fall through in a scene nobody draws.
    bool ground = renderer_ == nullptr;
    if (renderer_) {
        const collision::CollisionWorld& world = renderer_->collision();
        const auto floor = world.wmoFloor(collision::Reach::everywhere(), load.spawn.x, load.spawn.y,
                                          load.spawn.z + 5);
        ground = floor && std::abs(*floor - load.spawn.z) < 15;
        if (!ground) {
            const auto height = world.terrainHeight(load.spawn.x, load.spawn.y);
            ground = height && std::abs(*height - load.spawn.z) < 15;
        }
    }
    auto* terrain = renderer_ ? renderer_->getTerrainManager() : nullptr;
    if (ctx) ctx->pollUploadBatches();
    const bool wasReady = load.status.result == WorldLoadResult::Ready;
    load.status = load.warmup.observe(secondsSince(load.stageStarted), !spawnsPending, ground,
                                     terrain ? terrain->getLoadedTileCount() : 0, !ctx || ctx->uploadsComplete());
    if (!wasReady && load.status.result == WorldLoadResult::Ready && load.status.reason != "World ready")
        LOG_WARNING(load.status.reason, " (map=", load.entry.mapId, ")");
}

void WorldLoader::beginWarmup() {
    if (!requirePhase(phase_, Phase::Transitions, "begin world warmup") || stage() != Stage::GeometryReady) return;
    load_->stage = Stage::Warmup;
    load_->stageStarted = clock_();
    load_->status = {WorldLoadResult::Pending, "Finalizing world sync...", 0.97f};
    LOG_INFO("World geometry ready: map=", load_->entry.mapId, " elapsed_ms=", secondsSince(load_->started) * 1000);
}

bool WorldLoader::finish() {
    if (!requirePhase(phase_, Phase::Transitions, "finish world load") || status().result != WorldLoadResult::Ready) return false;
    if (auto* ctx = renderer_ ? renderer_->getVkContext() : nullptr) {
        ctx->pollUploadBatches();
        if (!ctx->uploadsComplete()) return false;
    }
    load_->stage = Stage::Finished;
    LOG_INFO("World load complete: map=", load_->entry.mapId, " total_ms=", secondsSince(load_->started) * 1000,
             " warmup_ms=", secondsSince(load_->stageStarted) * 1000);
    load_->prepared.reset();
    return true;
}

const char* WorldLoader::mapDisplayName(uint32_t mapId) {
    // Friendly display names for the loading screen
    switch (mapId) {
        case 0: return "Eastern Kingdoms";
        case 1: return "Kalimdor";
        case 13: return "Test";
        case 34: return "The Stockade";
        case 169: return "Emerald Dream";
        case 530: return "Outland";
        case 571: return "Northrend";
        default: return nullptr;
    }
}

const char* WorldLoader::mapIdToName(uint32_t mapId) {
    // Fallback when Map.dbc is unavailable. Names must match WDT directory names
    // (case-insensitive - AssetManager lowercases all paths).
    switch (mapId) {
        // Continents
        case 0: return "Azeroth";
        case 1: return "Kalimdor";
        case 530: return "Expansion01";
        case 571: return "Northrend";
        // Classic dungeons/raids
        case 30: return "PVPZone01";
        case 33: return "Shadowfang";
        case 34: return "StormwindJail";
        case 36: return "DeadminesInstance";
        case 43: return "WailingCaverns";
        case 47: return "RazserfenKraulInstance";
        case 48: return "Blackfathom";
        case 70: return "Uldaman";
        case 90: return "GnomeragonInstance";
        case 109: return "SunkenTemple";
        case 129: return "RazorfenDowns";
        case 189: return "MonasteryInstances";
        case 209: return "TanarisInstance";
        case 229: return "BlackRockSpire";
        case 230: return "BlackrockDepths";
        case 249: return "OnyxiaLairInstance";
        case 289: return "ScholomanceInstance";
        case 309: return "Zul'Gurub";
        case 329: return "Stratholme";
        case 349: return "Mauradon";
        case 369: return "DeeprunTram";
        case 389: return "OrgrimmarInstance";
        case 409: return "MoltenCore";
        case 429: return "DireMaul";
        case 469: return "BlackwingLair";
        case 489: return "PVPZone03";
        case 509: return "AhnQiraj";
        case 529: return "PVPZone04";
        case 531: return "AhnQirajTemple";
        case 533: return "Stratholme Raid";
        // TBC
        case 532: return "Karazahn";
        case 534: return "HyjalPast";
        case 540: return "HellfireMilitary";
        case 542: return "HellfireDemon";
        case 543: return "HellfireRampart";
        case 544: return "HellfireRaid";
        case 545: return "CoilfangPumping";
        case 546: return "CoilfangMarsh";
        case 547: return "CoilfangDraenei";
        case 548: return "CoilfangRaid";
        case 550: return "TempestKeepRaid";
        case 552: return "TempestKeepArcane";
        case 553: return "TempestKeepAtrium";
        case 554: return "TempestKeepFactory";
        case 555: return "AuchindounShadow";
        case 556: return "AuchindounDraenei";
        case 557: return "AuchindounEthereal";
        case 558: return "AuchindounDemon";
        case 560: return "HillsbradPast";
        case 564: return "BlackTemple";
        case 565: return "GruulsLair";
        case 566: return "PVPZone05";
        case 568: return "ZulAman";
        case 580: return "SunwellPlateau";
        case 585: return "Sunwell5ManFix";
        // WotLK
        case 574: return "Valgarde70";
        case 575: return "UtgardePinnacle";
        case 576: return "Nexus70";
        case 578: return "Nexus80";
        case 595: return "StratholmeCOT";
        case 599: return "Ulduar70";
        case 600: return "Ulduar80";
        case 601: return "DrakTheronKeep";
        case 602: return "GunDrak";
        case 603: return "UlduarRaid";
        case 608: return "DalaranPrison";
        case 615: return "ChamberOfAspectsBlack";
        case 617: return "DeathKnightStart";
        case 619: return "Azjol_Uppercity";
        case 624: return "WintergraspRaid";
        case 631: return "IcecrownCitadel";
        case 632: return "IcecrownCitadel5Man";
        case 649: return "ArgentTournamentRaid";
        case 650: return "ArgentTournamentDungeon";
        case 658: return "QuarryOfTears";
        case 668: return "HallsOfReflection";
        case 724: return "ChamberOfAspectsRed";
        default: return "";
    }
}
void WorldLoader::startWorldPreload(uint32_t mapId, const std::string& mapName,
                                     float serverX, float serverY) {
    cancelWorldPreload();
    if (!assetManager_ || !assetManager_->isInitialized() || mapName.empty()) return;

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, 0.0f));
    auto [tileX, tileY] = core::coords::canonicalToTile(canonical.x, canonical.y);

    worldPreload_ = std::make_unique<WorldPreload>();
    worldPreload_->mapId = mapId;
    worldPreload_->mapName = mapName;
    worldPreload_->centerTileX = tileX;
    worldPreload_->centerTileY = tileY;

    LOG_INFO("World preload: starting for map '", mapName, "' tile [", tileX, ",", tileY, "]");

    // Build list of tiles to preload (radius 1 = 3x3 = 9 tiles, matching load screen)
    struct TileJob { int x, y; };
    auto jobs = std::make_shared<std::vector<TileJob>>();
    // Center tile first (most important)
    jobs->push_back({tileX, tileY});
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            int tx = tileX + dx, ty = tileY + dy;
            if (tx < 0 || tx > 63 || ty < 0 || ty > 63) continue;
            jobs->push_back({tx, ty});
        }
    }

    // Spawn worker threads (one per tile for maximum parallelism)
    auto cancelFlag = &worldPreload_->cancel;
    auto* am = assetManager_;
    std::string mn = mapName;

    int numWorkers = std::min(static_cast<int>(jobs->size()), 4);
    auto nextJob = std::make_shared<std::atomic<int>>(0);

    for (int w = 0; w < numWorkers; w++) {
        worldPreload_->workers.emplace_back([am, mn, jobs, nextJob, cancelFlag]() {
            while (!cancelFlag->load(std::memory_order_relaxed)) {
                int idx = nextJob->fetch_add(1, std::memory_order_relaxed);
                if (idx >= static_cast<int>(jobs->size())) break;

                int tx = (*jobs)[idx].x;
                int ty = (*jobs)[idx].y;

                // Read ADT file (warms file cache)
                std::string adtPath = "World\\Maps\\" + mn + "\\" + mn + "_" +
                                      std::to_string(tx) + "_" + std::to_string(ty) + ".adt";
                // Discarded on purpose: the read is for the cache it warms.
                (void)am->readFileShared(adtPath);
                if (cancelFlag->load(std::memory_order_relaxed)) break;

                // Read obj0 variant
                std::string objPath = "World\\Maps\\" + mn + "\\" + mn + "_" +
                                      std::to_string(tx) + "_" + std::to_string(ty) + "_obj0.adt";
                (void)am->readFileShared(objPath);
            }
            LOG_DEBUG("World preload worker finished");
        });
    }
}

void WorldLoader::cancelWorldPreload() {
    if (!worldPreload_) return;
    worldPreload_->cancel.store(true, std::memory_order_relaxed);
    for (auto& t : worldPreload_->workers) {
        if (t.joinable()) t.join();
    }
    LOG_INFO("World preload: cancelled (map=", worldPreload_->mapName,
             " tile=[", worldPreload_->centerTileX, ",", worldPreload_->centerTileY, "])");
    worldPreload_.reset();
}

std::filesystem::path WorldLoader::defaultLastWorldFile() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    std::string dir = base ? std::string(base) + "\\wowee" : ".";
#else
    const char* home = std::getenv("HOME");
    std::string dir = home ? std::string(home) + "/.wowee" : ".";
#endif
    return std::filesystem::path(dir) / "last_world.cfg";
}

void WorldLoader::saveLastWorldInfo(uint32_t mapId, const std::string& mapName,
                                     float serverX, float serverY) {
    std::error_code ec;
    if (lastWorldFile_.has_parent_path()) std::filesystem::create_directories(lastWorldFile_.parent_path(), ec);
    std::ofstream f(lastWorldFile_);
    if (f) {
        f << mapId << "\n" << mapName << "\n" << serverX << "\n" << serverY << "\n";
    } else if (!std::exchange(reportedLastWorldFailure_, true)) {
        LOG_WARNING("Could not write ", lastWorldFile_.string(),
                    ": the next start will not preload the map last entered");
    }
}

WorldLoader::LastWorldInfo WorldLoader::loadLastWorldInfo() const {
    LastWorldInfo info;
    std::ifstream f(lastWorldFile_);
    if (!f) return info;
    std::string line;
    try {
        if (std::getline(f, line)) info.mapId = static_cast<uint32_t>(std::stoul(line));
        if (std::getline(f, line)) info.mapName = line;
        if (std::getline(f, line)) info.x = std::stof(line);
        if (std::getline(f, line)) info.y = std::stof(line);
    } catch (...) {
        LOG_WARNING("Malformed last_world.cfg, ignoring saved position");
        return info;
    }
    info.valid = !info.mapName.empty();
    return info;
}

} // namespace wowee::core
