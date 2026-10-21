#include "core/scene_replay.hpp"
#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "core/player_view.hpp"
#include "core/zone_tracker.hpp"
#include "game/entity_controller.hpp"
#include "game/game_handler.hpp"
#include "game/game_events.hpp"
#include "game/inventory_handler.hpp"
#include "game/expansion_profile.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/mpq_archive_layer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/frame_inputs.hpp"
#include "rendering/frame_scene.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/weather_controls.hpp"
#include "rendering/post_process_pipeline.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace wowee::core {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
double stamp() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }
std::optional<double> threadCpu() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)) {
        const auto ticks=[](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime; };
        return static_cast<double>(ticks(kernel)+ticks(user))/10000.0;
    }
#else
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&time)==0)
        return static_cast<double>(time.tv_sec)*1000.0+static_cast<double>(time.tv_nsec)/1000000.0;
#endif
    static std::atomic<bool> reported=false;
    if (!reported.exchange(true)) LOG_WARNING("Thread CPU clock unavailable; trace keeps wall times and leaves CPU time absent");
    return std::nullopt;
}
std::string environment(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "";
}
float seconds(const char* name, float fallback) {
    const auto text = environment(name);
    if (text.empty()) return fallback;
    size_t used = 0;
    const float value = std::stof(text, &used);
    if (used != text.size() || !std::isfinite(value) || value <= 0 || value > 300)
        throw std::runtime_error(std::string(name) + " must be above zero and at most 300 seconds");
    return value;
}
Json vec(const glm::vec3& v) { return Json::array({v.x, v.y, v.z}); }
glm::vec3 vec(const Json& v) { return {v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()}; }
void writeJson(const std::filesystem::path& path, const Json& value) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".part";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open " + temporary);
    out << value;
    out.close();
    if (!out) throw std::runtime_error("cannot finish " + temporary);
    // Refuse replacement: a capture or A/B report must not silently erase evidence.
    if (std::filesystem::exists(path)) throw std::runtime_error("output already exists: " + path.string());
    std::filesystem::rename(temporary, path);
}
Json signature(const std::filesystem::path& path) {
    return {{"path", path.string()}, {"bytes", std::filesystem::file_size(path)},
        {"modified", std::filesystem::last_write_time(path).time_since_epoch().count()}};
}
void verifySignature(const Json& expected) {
    const auto path = expected.at("path").get<std::string>();
    if (signature(path) != expected) throw std::runtime_error("capture asset changed: " + path);
}
Json characterJson(const game::Character& c) {
    Json gear = Json::array();
    for (const auto& item : c.equipment) gear.push_back({item.displayModel, item.inventoryType, item.enchantment});
    return {{"guid", c.guid}, {"race", c.race}, {"class", c.characterClass}, {"gender", c.gender},
        {"female", c.useFemaleModel}, {"appearance", c.appearanceBytes}, {"facial", c.facialFeatures},
        {"level", c.level}, {"map", c.mapId}, {"zone", c.zoneId}, {"position", {c.x,c.y,c.z}}, {"gear", gear}};
}
game::Character characterFrom(const Json& j) {
    game::Character c{};
    c.guid = j.at("guid"); c.name = "SceneReplay";
    c.race = j.at("race").get<game::Race>(); c.characterClass = j.at("class").get<game::Class>();
    c.gender = j.at("gender").get<game::Gender>(); c.useFemaleModel = j.at("female");
    c.appearanceBytes = j.at("appearance"); c.facialFeatures = j.at("facial"); c.level = j.at("level");
    c.mapId = j.at("map"); c.zoneId = j.at("zone");
    const auto p = vec(j.at("position")); c.x=p.x; c.y=p.y; c.z=p.z;
    for (const auto& item : j.at("gear")) c.equipment.push_back({item.at(0), item.at(1), item.at(2)});
    return c;
}
Json statistics(const rendering::RenderStatistics& s) {
    return {{"terrainDrawn", s.terrainDrawn}, {"terrainCulled", s.terrainCulled},
        {"terrainTriangles",s.terrainTriangles}, {"m2Draws",s.m2Draws}, {"wmoDraws",s.wmoDraws},
        {"wmoShadows",s.wmoShadows}, {"wmoDistanceCulled",s.wmoDistanceCulled},
        {"wmoPortalCulled",s.wmoPortalCulled}, {"rtInstances",s.rtInstances}, {"rtMeshes",s.rtMeshes},
        {"rtTriangles",s.rtTriangles}, {"rtGpuBytes",s.rtGpuBytes}, {"rtSourceBytes",s.rtSourceBytes},
        {"width",s.width}, {"height",s.height}};
}
}

struct SceneReplay::State {
    std::atomic<bool> tracing{false};
    std::mutex mutex;
    std::map<uint64_t, Json> rows;
    Json stages = Json::array();
    double producerStart = 0;
    std::optional<double> producerCpu;
    std::string tracePath, capturePath;
    Json capture, replay;
    std::unordered_map<uint64_t, game::FlatFieldMap> previousFields;
    std::unordered_map<uint64_t, game::ObjectType> previousTypes;
    std::map<uint64_t, Json> gear;
    std::set<uint64_t> skippedGear;
    Json pendingEvents = Json::array();
    std::vector<EventBus::Subscription> subscriptions;
    std::future<void> writer;
    bool capturing = false, armed = false, captureDone = false, replaying = false;
    bool entered = false, seeded = false, playing = false, readyToPlay = false, finishing = false, saved = false;
    float captureSeconds = 30, elapsed = 0, settleSeconds = 10, settled = 0, warmup = 0;
    size_t sample = 0;
    std::optional<size_t> appliedSample;
    uint64_t captureLastSequence = 0;
    std::string error;
    void saveTrace() {
        if (saved || tracePath.empty()) return;
        Json frames = Json::array();
        { std::lock_guard lock(mutex); for (auto& [id, row] : rows) frames.push_back(std::move(row)); }
        Json report{{"version",1}, {"clock","steady_clock milliseconds; wall durations, not CPU cycles"},
            {"renderThread",environment("WOWEE_RENDER_THREAD")}, {"scene",environment("WOWEE_SCENE_REPLAY")},
            {"error",error}, {"frames",std::move(frames)}};
        writeJson(tracePath, report);
        saved = true;
        LOG_WARNING("Performance trace saved: ", tracePath);
    }
};

SceneReplay::SceneReplay() : state_(std::make_unique<State>()) {}
SceneReplay::~SceneReplay() {
    try {
        if (state_->writer.valid()) state_->writer.get();
        if (state_->capturing) LOG_ERROR("Scene capture interrupted; no complete replay was saved");
        state_->saveTrace();
    } catch (const std::exception& e) { LOG_ERROR("Scene/performance recording failed: ", e.what()); }
}

void SceneReplay::initialize(Application& app) {
    auto& s = *state_;
    try {
        s.tracePath = environment("WOWEE_PERF_TRACE");
        s.capturePath = environment("WOWEE_SCENE_CAPTURE");
        s.armed = !s.capturePath.empty();
        if (s.armed && s.tracePath.empty()) s.tracePath=s.capturePath+".perf.json";
        s.tracing=!s.tracePath.empty();
        s.captureSeconds = seconds("WOWEE_SCENE_CAPTURE_SECONDS",30);
        s.settleSeconds = seconds("WOWEE_SCENE_REPLAY_SETTLE",10);
        const auto replayPath = environment("WOWEE_SCENE_REPLAY");
        if (!replayPath.empty()) {
            if (s.armed || app.benchmark_) throw std::runtime_error("scene replay cannot run with capture or a benchmark route");
            std::ifstream in(replayPath, std::ios::binary);
            if (!in) throw std::runtime_error("cannot open scene replay " + replayPath);
            s.replay = Json::parse(in);
            if (s.replay.at("version") != 1 || s.replay.at("frames").empty() || !s.replay.at("complete").get<bool>())
                throw std::runtime_error("unsupported or incomplete scene recording");
            if (app.expansionRegistry_->getActiveId()!=s.replay.at("expansion").get<std::string>())
                throw std::runtime_error("replay expansion differs from the captured expansion");
            for (const auto& frame : s.replay.at("frames")) {
                const float dt=frame.at("dt");
                if (!std::isfinite(dt) || dt<=0 || dt>10) throw std::runtime_error("invalid sampled frame delta");
                for (const char* name : {"eye","forward","player"}) {
                    const auto p=vec(frame.at(name));
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                        throw std::runtime_error("non-finite recorded position");
                }
            }
            verifySignature(s.replay.at("manifest"));
            if (app.assetManager->getDataPath() != s.replay.at("dataPath").get<std::string>())
                throw std::runtime_error("replay data root differs; use the captured dataPath");
            Application::RealmPatchSet patches;
            patches.realm = s.replay.at("realm");
            for (const auto& archive : s.replay.at("archives")) {
                verifySignature(archive);
                patches.archives.emplace_back(archive.at("path").get<std::string>());
            }
            if (!patches.archives.empty()) {
#ifdef WOWEE_HAVE_STORMLIB
                std::string error;
                auto layer = pipeline::MpqArchiveLayer::open(patches.archives,error);
                if (!layer) throw std::runtime_error("cannot mount captured realm archives: " + error);
                app.applyRealmPatchLayer(std::move(layer),std::move(patches));
#else
                throw std::runtime_error("captured realm archives require a StormLib build");
#endif
            }
            for (const auto& entry : s.replay.at("fields"))
                app.gameHandler->updateFieldTableRef().setIndex(static_cast<game::UF>(entry.at(0).get<uint16_t>()),entry.at(1));
            if (s.tracePath.empty()) s.tracePath = replayPath + ".perf.json";
            s.tracing=true;
            s.replaying = true;
            LOG_WARNING("Offline scene replay: ", replayPath, ", ", s.replay.at("frames").size(),
                " samples; source updates run before ordinary presentation/extraction; no realm login");
        }
        s.subscriptions.push_back(app.eventBus_.subscribe<game::events::PlayerEquipmentResolved>([this](const auto& e) {
            auto& st = *state_;
            if (st.captureDone) return;
            Json value{{"guid",e.guid},{"displays",e.displayIds},{"types",e.inventoryTypes},{"enchants",e.enchantIds}};
            st.gear[e.guid] = value;
            if (st.capturing) st.pendingEvents.push_back({{"kind","gear"},{"value",std::move(value)}});
        }));
        s.subscriptions.push_back(app.eventBus_.subscribe<game::events::UnitEmotePlayed>([this](const auto& e) {
            if (state_->capturing) state_->pendingEvents.push_back({{"kind","emote"},{"guid",e.guid},
                {"animation",e.animationId},{"loop",e.looping}});
        }));
        s.subscriptions.push_back(app.eventBus_.subscribe<game::events::SpellVisualRequested>([this](const auto& e) {
            if (state_->capturing) state_->pendingEvents.push_back({{"kind","visual"},{"guid",e.unitGuid},
                {"visual",e.visualId},{"impact",e.impactKit}});
        }));
        s.subscriptions.push_back(app.eventBus_.subscribe<game::events::SpellCastStarted>([this](const auto& e) {
            if (state_->capturing) state_->pendingEvents.push_back({{"kind","cast"},{"guid",e.casterGuid},
                {"spell",e.spellId},{"duration",e.castTimeMs},{"player",e.byPlayer},
                {"profession",e.isProfession},{"ranged",e.isRangedAutoAttack}});
        }));
        s.subscriptions.push_back(app.eventBus_.subscribe<game::events::SpellCastSucceeded>([this](const auto& e) {
            if (state_->capturing) state_->pendingEvents.push_back({{"kind","spell"},{"guid",e.casterGuid},
                {"spell",e.spellId},{"player",e.byPlayer},{"profession",e.isProfession},
                {"ranged",e.isRangedAutoAttack},{"melee",e.isMeleeAbility},{"hits",e.hitTargets},{"projectile",e.projectileTarget}});
        }));
        s.subscriptions.push_back(app.eventBus_.subscribe<game::events::CastAnimationChanged>([this](const auto& e) {
            if (state_->capturing) state_->pendingEvents.push_back({{"kind","castAnimation"},{"guid",e.casterGuid},
                {"started",e.started},{"channel",e.channel},{"type",e.castType}});
        }));
        // A manually created observer joins an already running character.
        // Startup observers hear the original resolutions as they arrive.
        if (app.getState()==AppState::IN_GAME) app.gameHandler->inventory().emitAllOtherPlayerEquipment();
        if (s.armed) LOG_WARNING("Scene capture armed: ",s.capturePath,"; starts three seconds after world entry. Ctrl+F9 starts/stops manually");
    } catch (const std::exception& e) {
        s.error = e.what(); LOG_ERROR("Scene replay initialization failed: ",e.what()); app.requestClose();
    }
}

void SceneReplay::prepare(Application& app) {
    auto& s = *state_;
    if (s.finishing) {
        try { s.saveTrace(); } catch (const std::exception& e) { s.error=e.what(); LOG_ERROR("Cannot save replay trace: ",e.what()); }
        app.requestClose();
        return;
    }
    if (!s.replaying || s.entered) return;
    s.entered = true;
    app.beginOfflineWorldEntry(characterFrom(s.replay.at("character")));
}

float SceneReplay::deltaTime(float actual) const {
    const auto& s = *state_;
    return s.replaying && s.playing && !s.finishing ? s.replay.at("frames").at(s.sample).at("dt").get<float>() : actual;
}

void SceneReplay::beforeGame(Application& app) {
    auto& s = *state_;
    if (!s.replaying || s.finishing || (s.seeded && !s.playing)) return;
    const auto& frame = s.replay.at("frames").at(s.sample);
    game::UpdateObjectData update;
    update.outOfRangeGuids = frame.at("removed").get<std::vector<uint64_t>>();
    for (const auto& object : frame.at("objects")) {
        game::UpdateBlock block{};
        block.guid = object.at("guid"); block.objectType = object.at("type").get<game::ObjectType>();
        block.updateType = object.at("create").get<bool>() ? game::UpdateType::CREATE_OBJECT : game::UpdateType::VALUES;
        block.hasMovement = object.at("positioned");
        const auto p = vec(object.at("position"));
        const auto server = coords::canonicalToServer(p);
        block.x=server.x; block.y=server.y; block.z=server.z;
        block.orientation=coords::canonicalToServerYaw(object.at("orientation").get<float>());
        block.moveFlags=object.at("flags");
        for (const auto& field : object.at("fields")) block.fields[field.at(0).get<uint16_t>()]=field.at(1);
        update.blocks.push_back(std::move(block));
    }
    if (s.appliedSample != s.sample) app.gameHandler->getEntityController()->applyRecordedObjects(update);
    game::UpdateObjectData objectMotion;
    for (const auto& pose : frame.at("poses")) {
        const uint64_t guid = pose.at(0);
        if (auto entity = app.gameHandler->getEntityManager().getEntity(guid)) {
            entity->applySampledPosition(pose.at(1),pose.at(2),pose.at(3),pose.at(4),pose.at(5));
            const uint32_t flags=pose.at(6);
            const auto flag=[guid,flags](auto& state,uint32_t mask) {
                if (flags & mask) state[guid]=true; else state.erase(guid);
            };
            flag(app.entitySpawner_->getCreatureWalkingState(),0x100);
            flag(app.entitySpawner_->getCreatureSwimmingState(),0x200000);
            flag(app.entitySpawner_->getCreatureFlyingState(),0x1000000);
            if (entity->getType()==game::ObjectType::GAMEOBJECT) {
                game::UpdateBlock block{};
                block.guid=guid; block.objectType=game::ObjectType::GAMEOBJECT;
                block.updateType=game::UpdateType::MOVEMENT; block.hasMovement=true;
                const auto p=coords::canonicalToServer({entity->getX(),entity->getY(),entity->getZ()});
                block.x=p.x; block.y=p.y; block.z=p.z;
                block.orientation=coords::canonicalToServerYaw(entity->getOrientation());
                objectMotion.blocks.push_back(std::move(block));
            }
        } else { throw std::runtime_error("scene pose refers to absent entity " + std::to_string(guid)); }
    }
    if (!objectMotion.blocks.empty()) app.gameHandler->getEntityController()->applyRecordedObjects(objectMotion);
    const auto applyGear = [&app,&s](const Json& value) {
        const uint64_t guid=value.at("guid");
        if (guid==app.gameHandler->getPlayerGuid() || !app.gameHandler->getEntityManager().hasEntity(guid)) {
            if (s.skippedGear.insert(guid).second) LOG_WARNING("Scene replay: remote equipment for guid ",guid,
                " has no remote player; event omitted (local equipment comes from the character snapshot)");
            return;
        }
        EntitySpawner::WornEquipment worn;
        worn.displayInfoIds=value.at("displays").get<std::array<uint32_t,19>>();
        worn.inventoryTypes=value.at("types").get<std::array<uint8_t,19>>();
        worn.enchantIds=value.at("enchants").get<std::array<uint32_t,19>>();
        app.entitySpawner_->queuePlayerEquipment(value.at("guid"),worn);
    };
    if (!s.seeded) for (const auto& value : s.replay.at("gear")) applyGear(value);
    if (s.appliedSample != s.sample) for (const auto& event : frame.at("events")) {
        const std::string kind=event.at("kind");
        if (kind=="gear") applyGear(event.at("value"));
        else if (kind=="emote") app.eventBus_.publish(game::events::UnitEmotePlayed{event.at("guid"),event.at("animation"),event.at("loop")});
        else if (kind=="visual") app.eventBus_.publish(game::events::SpellVisualRequested{event.at("guid"),event.at("visual"),event.at("impact")});
        else if (kind=="cast") app.eventBus_.publish(game::events::SpellCastStarted{event.at("guid"),event.at("spell"),event.at("duration"),
            event.at("player"),event.at("profession"),event.at("ranged")});
        else if (kind=="spell") app.eventBus_.publish(game::events::SpellCastSucceeded{event.at("guid"),event.at("spell"),
            event.at("player"),event.at("profession"),event.at("ranged"),event.at("melee"),
            event.at("hits").get<std::vector<uint64_t>>(),event.at("projectile")});
        else if (kind=="castAnimation") app.eventBus_.publish(game::events::CastAnimationChanged{event.at("guid"),event.at("started"),
            event.at("channel"),event.at("type").get<game::SpellCastType>()});
        else throw std::runtime_error("unsupported recorded event " + kind);
    }
    s.seeded=true;
    s.appliedSample=s.sample;
}

void SceneReplay::placeCamera(Application& app) {
    auto& s=*state_;
    if (!s.replaying || s.finishing) return;
    const auto& frame=s.replay.at("frames").at(s.sample);
    auto* camera=app.renderer->getCamera();
    camera->setPosition(vec(frame.at("eye")));
    const auto forward=vec(frame.at("forward"));
    camera->setRotation(glm::degrees(std::atan2(forward.y,forward.x)),glm::degrees(std::asin(std::clamp(forward.z,-1.0f,1.0f))));
    camera->setFov(frame.at("fov"));
    const auto player=vec(frame.at("player"));
    app.renderer->getCharacterPosition()=player;
    auto& movement=app.gameHandler->movementInfoRef();
    const auto canonical=coords::renderToCanonical(player);
    movement.x=canonical.x; movement.y=canonical.y; movement.z=canonical.z;
    app.renderer->getLightingManager()->setTimeOfDay(frame.at("hour").get<float>()/24.0f);
    auto& weather=app.renderer->weatherControls();
    weather.weather=rendering::WeatherTarget{frame.at("weather").at(0).get<pipeline::WeatherKind>(),frame.at("weather").at(1),true};
    const auto& settings=s.replay.at("renderSettings");
    if (auto* post=app.renderer->getPostProcessPipeline()) post->setBloomStrength(settings.value("bloom",0.0f));
    app.renderer->setViewDistance(settings.at("viewDistance"));
    app.renderer->setRtLightingMode(settings.at("rtMode"));
    app.renderer->setRtDistance(settings.at("rtDistance"));
    app.renderer->setVolumetricFogQuality(settings.at("fogQuality"));
    app.renderer->setVolumetricFogDensity(settings.at("fogDensity"));
    app.renderer->setMsaaSamples(static_cast<VkSampleCountFlagBits>(settings.at("msaa").get<uint32_t>()));
    auto world=rendering::WorldEnvironment{};
    world.mapId=s.replay.at("character").at("map"); world.zoneId=frame.at("zone");
    world.areaId=frame.at("area"); world.gameTime=frame.at("hour"); world.realmSeconds=frame.at("realmSeconds");
    world.weatherKind=weather.weather->kind; world.weatherIntensity=weather.weather->intensity; world.weatherAbrupt=true;
    app.renderer->setWorldEnvironment(world);
    app.window->setFrameCap(0);
    if (app.window->isVsyncEnabled()) app.window->setVsync(false);
}

void SceneReplay::toggleCapture(Application& app) {
    auto& s=*state_;
    if (s.replaying) { LOG_WARNING("Scene capture ignored during replay"); return; }
    if (s.capturing) { s.capturing=false; s.captureDone=true; return; }
    if (s.captureDone || s.writer.valid()) { LOG_WARNING("This launch already recorded a scene; restart with a new capture path"); return; }
    if (app.getState()!=AppState::IN_GAME || !app.gameHandler->getActiveCharacter()) {
        LOG_WARNING("Scene capture needs a character in the world; no recording started"); return;
    }
    try {
        if (s.capturePath.empty()) s.capturePath="logs/scene-"+std::to_string(static_cast<uint64_t>(stamp()))+".json";
        if (std::filesystem::exists(s.capturePath)) throw std::runtime_error("capture output already exists: "+s.capturePath);
        s.capture={{"version",1},{"complete",false},{"frames",Json::array()},
            {"expansion",app.expansionRegistry_->getActiveId()},
            {"dataRoot",std::filesystem::absolute(environment("WOW_DATA_PATH").empty() ? "Data" : environment("WOW_DATA_PATH")).lexically_normal().string()},
            {"dataPath",app.assetManager->getDataPath()}, {"manifest",signature(std::filesystem::path(app.assetManager->getDataPath())/"manifest.json")},
            {"realm",app.mountedRealmPatches_.realm},{"archives",Json::array()}, {"fields",Json::array()},
            {"gear",Json::array()}, {"config",Json::object()},
            {"limitations",{"Existing transient spell particles and animation phase are not keyframed; interface and network load are not replayed. Compare scene counters and control images before accepting an A/B."}}};
        auto character=*app.gameHandler->getActiveCharacter();
        uint8_t race=0,gender=0,facial=0; uint32_t appearance=0;
        if (app.gameHandler->getEntityController()->getPlayerAppearance(character.guid,race,gender,appearance,facial)) {
            character.race=static_cast<game::Race>(race); character.gender=static_cast<game::Gender>(gender);
            character.appearanceBytes=appearance; character.facialFeatures=facial;
        }
        std::array<uint32_t,19> displays{}; std::array<uint8_t,19> types{};
        if (app.gameHandler->inventory().resolvePlayerEquipment(displays,types)) {
            character.equipment.resize(19);
            for (size_t slot=0;slot<19;++slot) character.equipment[slot]={displays[slot],types[slot],
                app.gameHandler->inventory().shownEnchantId(app.gameHandler->inventory().getEquipSlotGuid(static_cast<int>(slot)))};
        }
        const auto p=coords::canonicalToServer(coords::renderToCanonical(app.renderer->getCharacterPosition()));
        character.mapId=app.gameHandler->getCurrentMapId(); character.x=p.x; character.y=p.y; character.z=p.z;
        s.capture["character"]=characterJson(character);
        for (const auto& path : app.mountedRealmPatches_.archives) s.capture["archives"].push_back(signature(path));
        for (uint16_t id=0; id<static_cast<uint16_t>(game::UF::COUNT); ++id) {
            const auto field=static_cast<game::UF>(id);
            if (app.gameHandler->updateFieldTableRef().hasField(field))
                s.capture["fields"].push_back({id,app.gameHandler->updateFieldTableRef().index(field)});
        }
        for (const auto& [guid,value] : s.gear) {
            const auto entity=app.gameHandler->getEntityManager().getEntity(guid);
            if (entity && entity->getType()==game::ObjectType::PLAYER && guid!=character.guid)
                s.capture["gear"].push_back(value);
        }
        for (const char* name : {"settings.cfg","cvars.cfg"}) {
            std::ifstream in(std::filesystem::path(getConfigRoot())/name,std::ios::binary);
            if (!in) throw std::runtime_error(std::string("cannot capture graphics config ")+name);
            s.capture["config"][name]=std::string(std::istreambuf_iterator<char>(in),{});
        }
        s.capturing=true; s.armed=false; s.elapsed=0;
        if (s.tracePath.empty()) s.tracePath=s.capturePath+".perf.json";
        s.tracing=true;
        LOG_WARNING("Recording producer scene for ",s.captureSeconds," seconds to ",s.capturePath,
            ". Capture sampling overhead is reported separately; use offline replay for performance A/B");
    } catch (const std::exception& e) { s.armed=false; LOG_ERROR("Scene capture cannot start: ",e.what()); }
}

void SceneReplay::sampled(Application& app,const rendering::FrameInputs& input) {
    auto& s=*state_;
    if (!s.tracing && !s.armed && !s.capturing && !s.replaying) return;
    if (!input.scene) return;
    const bool world=app.getState()==AppState::IN_GAME;
    const auto* terrain=app.renderer->getTerrainManager();
    const bool busy=app.entitySpawner_->hasWorkPending() || (terrain && terrain->getRemainingTileCount()>0);
    if (s.armed && world) {
        // A real crowd can keep spawning continuously. Its source state is
        // recordable while presentation assets are still loading.
        s.settled+=input.deltaTime;
        if (s.settled>=3) toggleCapture(app);
    }
    if (s.replaying && world && s.seeded && !s.playing && !s.finishing) {
        s.warmup+=input.deltaTime;
        s.settled=busy ? 0 : s.settled+input.deltaTime;
        if (s.settled>=s.settleSeconds || s.warmup>=std::max(20.0f,s.settleSeconds*2)) {
            s.readyToPlay=true;
            if (busy) LOG_WARNING("Scene replay warmup budget reached while assets still load; measuring approximate workload and reporting residency differences");
            LOG_WARNING("Scene replay warmed; measuring ",s.replay.at("frames").size()," source samples");
        }
    }
    Json resident{{"m2",app.renderer->getM2Renderer() ? app.renderer->getM2Renderer()->getInstanceCount() : 0},
        {"wmo",app.renderer->getWMORenderer() ? app.renderer->getWMORenderer()->getInstanceCount() : 0},
        {"characters",app.renderer->getCharacterRenderer() ? app.renderer->getCharacterRenderer()->getInstanceCount() : 0},
        {"tiles",terrain ? terrain->getLoadedTileCount() : 0},
        {"entities",app.gameHandler->getEntityManager().getEntityCount()}};
    {
        std::lock_guard lock(s.mutex);
        if (s.tracing) {
            auto& row=s.rows[input.scene->sequence];
            row["resident"]=resident;
            row["sourceSample"]=s.replaying ? Json(s.sample) : Json(nullptr);
            row["measured"]=s.replaying ? s.playing && !s.finishing : world;
            if (const auto heap=app.renderer->getVkContext()->deviceLocalHeap()) {
                row["vramUsage"]=heap->usageBytes; row["vramBudget"]=heap->budgetBytes;
            }
            if (s.replaying && s.playing) row["expectedResident"]=s.replay.at("frames").at(s.sample).at("resident");
        }
    }
    if (!s.capturing || !world) return;
    if (app.gameHandler->getCurrentMapId()!=s.capture.at("character").at("map").get<uint32_t>()) {
        LOG_ERROR("Scene capture crossed a map boundary; incomplete recording discarded");
        s.capturing=false; s.captureDone=false; s.capture=Json(); return;
    }
    const double start=stamp();
    Json frame{{"dt",input.deltaTime},{"sequence",input.scene->sequence},{"eye",vec(input.camera.getPosition())},
        {"forward",vec(input.camera.getForward())},{"fov",input.camera.getFovDegrees()},
        {"player",vec(input.characterPosition)},{"hour",input.timeOfDay},
        {"zone",input.environment.zoneId},{"area",input.environment.areaId},{"realmSeconds",input.environment.realmSeconds},
        {"weather",{app.renderer->weatherTarget().kind,app.renderer->weatherTarget().intensity}},
        {"resident",std::move(resident)},{"objects",Json::array()},{"removed",Json::array()},
        {"poses",Json::array()},{"events",std::move(s.pendingEvents)}};
    s.pendingEvents=Json::array();
    if (s.capture.at("frames").empty()) s.capture["renderSettings"]={{"viewDistance",input.viewDistance},
        {"rtMode",input.rtMode},{"rtDistance",input.rtDistance},{"fogQuality",input.fogQuality},
        {"fogDensity",input.fogDensity},{"msaa",input.msaaSamples},{"bloom",input.postProcess.bloom},
        {"width",input.drawableWidth},{"height",input.drawableHeight}};
    const auto& entities=app.gameHandler->getEntityManager().getEntities();
    for (const auto& [guid,entity] : entities) {
        if (!entity) continue;
        const bool created=!s.previousTypes.contains(guid) || s.previousTypes.at(guid)!=entity->getType();
        Json changed=Json::array();
        auto& previous=s.previousFields[guid];
        for (const auto& [index,value] : entity->getFields()) {
            const auto old=previous.find(index);
            if (created || old==previous.end() || old->second!=value) changed.push_back({index,value});
        }
        uint32_t flags=0;
        // Locomotion flags retained by the presentation are also needed when
        // the server supplied a spline rather than a ReportedMovement.
        if (app.entitySpawner_->getCreatureWalkingState().contains(guid)) flags|=0x100;
        if (app.entitySpawner_->getCreatureSwimmingState().contains(guid)) flags|=0x200000;
        if (app.entitySpawner_->getCreatureFlyingState().contains(guid)) flags|=0x1000000;
        if (created || !changed.empty()) {
            frame["objects"].push_back({{"guid",guid},{"type",entity->getType()},{"create",created},
                {"positioned",entity->hasPosition()}, {"position",{entity->getX(),entity->getY(),entity->getZ()}},
                {"orientation",entity->getOrientation()},{"flags",flags},{"fields",std::move(changed)}});
        }
        if (entity->hasPosition()) frame["poses"].push_back({guid,entity->getX(),entity->getY(),entity->getZ(),
            entity->getOrientation(),entity->isEntityMoving(),flags});
        previous=entity->getFields(); s.previousTypes[guid]=entity->getType();
    }
    for (auto it=s.previousTypes.begin(); it!=s.previousTypes.end();) {
        if (!entities.contains(it->first)) {
            frame["removed"].push_back(it->first); s.previousFields.erase(it->first); it=s.previousTypes.erase(it);
        } else ++it;
    }
    s.elapsed+=input.deltaTime;
    s.captureLastSequence=input.scene->sequence;
    s.capture["frames"].push_back(std::move(frame));
    stage("capture/source sampling",static_cast<float>(stamp()-start));
    if (s.elapsed>=s.captureSeconds || s.capture.at("frames").size()>=18000) { s.capturing=false; s.captureDone=true; }
}

void SceneReplay::producerStarted() {
    auto& s=*state_; if (!s.tracing) return;
    s.producerStart=stamp(); s.stages=Json::array();
    s.producerCpu=threadCpu();
}
void SceneReplay::stage(std::string_view name,float ms) {
    auto& s=*state_; if (!s.tracing) return;
    if (name.starts_with("render thread")) return;
    s.stages.push_back({{"name",name},{"ms",ms}});
}
void SceneReplay::extracted(uint64_t sequence,double queueWaitMs,size_t outstanding) {
    auto& s=*state_; if (!s.tracing) return;
    std::lock_guard lock(s.mutex);
    if (s.rows.size()>=200000) throw std::runtime_error("performance trace exceeds 200000 frames; stop this run");
    auto& row=s.rows[sequence];
    row["sequence"]=sequence; row["producerStart"]=s.producerStart; row["extracted"]=stamp();
    row["queueWaitMs"]=queueWaitMs; row["outstanding"]=outstanding;
}
void SceneReplay::submitted(uint64_t sequence) {
    auto& s=*state_; if (!s.tracing) return;
    std::lock_guard lock(s.mutex); s.rows[sequence]["submitted"]=stamp();
    s.rows[sequence]["producerStages"]=std::move(s.stages); s.stages=Json::array();
    if (const auto cpu=threadCpu(); cpu && s.producerCpu) s.rows[sequence]["producerCpuMs"]=*cpu-*s.producerCpu;
}
void SceneReplay::recordingStarted(uint64_t sequence) {
    auto& s=*state_; if (!s.tracing) return;
    std::lock_guard lock(s.mutex); s.rows[sequence]["recordStart"]=stamp();
    if (const auto cpu=threadCpu()) s.rows[sequence]["recordCpuStart"]=*cpu;
}
void SceneReplay::recordingFinished(uint64_t sequence) {
    auto& s=*state_; if (!s.tracing) return;
    std::lock_guard lock(s.mutex); s.rows[sequence]["recordEnd"]=stamp();
    if (const auto cpu=threadCpu(); cpu && s.rows[sequence].contains("recordCpuStart"))
        s.rows[sequence]["recordCpuMs"]=*cpu-s.rows[sequence].at("recordCpuStart").get<double>();
}
void SceneReplay::result(const rendering::FrameResults& result) {
    auto& s=*state_; if (!s.tracing) return;
    std::lock_guard lock(s.mutex);
    auto& row=s.rows[result.gameFrame];
    row["resultReceived"]=stamp(); row["gpuSubmission"]=result.submittedFrame;
    row["gpuTimingFrame"]=result.gpuTimingFrame; row["gpuTimingSerial"]=result.gpuTimingSerial;
    row["gpu"]=result.gpu; row["statistics"]=statistics(result.statistics);
    row["recordStages"]=Json::array();
    for (const auto& part : result.cpu) row["recordStages"].push_back({{"name",part.group+"/"+part.name},{"ms",part.milliseconds}});
}
void SceneReplay::frameEnded(Application& app) {
    auto& s=*state_;
    if (s.readyToPlay) { s.readyToPlay=false; s.playing=true; return; }
    if (s.captureDone && !s.writer.valid() && !s.capture.is_null() && !s.capture.at("frames").empty()) {
        // Wait until the mailbox delivered the final captured recording. No
        // disk formatting or writing runs in a measured game frame.
        std::lock_guard lock(s.mutex);
        if (s.rows.contains(s.captureLastSequence) && s.rows.at(s.captureLastSequence).contains("statistics")) {
            for (auto& frame : s.capture["frames"]) {
                const auto id=frame.at("sequence").get<uint64_t>();
                if (s.rows.contains(id) && s.rows.at(id).contains("statistics")) frame["expectedDraws"]=s.rows.at(id).at("statistics");
            }
            s.capture["complete"]=true;
            const auto path=s.capturePath;
            s.tracing=false;
            Json rows=Json::array();
            for (auto& [id,row] : s.rows) rows.push_back(std::move(row));
            s.rows.clear();
            s.previousFields.clear(); s.previousTypes.clear(); s.gear.clear();
            s.pendingEvents=Json::array();
            Json trace{{"version",1},{"scene",""},{"error",""},{"frames",std::move(rows)}};
            s.saved=true;
            const auto tracePath=s.tracePath;
            s.writer=std::async(std::launch::async,[path,tracePath,document=std::move(s.capture),trace=std::move(trace)] {
                try { writeJson(path,document); writeJson(tracePath,trace); LOG_WARNING("Scene capture and metrics saved: ",path); }
                catch (const std::exception& e) { LOG_ERROR("Scene capture write failed: ",e.what()); throw; }
            });
        }
    }
    if (s.replaying && s.playing && !s.finishing) {
        if (++s.sample>=s.replay.at("frames").size()) {
            app.renderWorker_->drain();
            // beginFrame consumes the final results before prepare closes.
            s.finishing=true;
            LOG_WARNING("Scene replay finished; collecting final recording results");
        }
    }
}
}
