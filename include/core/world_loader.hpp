#pragma once

#include "core/frame_plan.hpp"
#include "core/world_load_progress.hpp"
#include "core/job_group.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <atomic>
#include <thread>
#include <cstdint>

namespace wowee {

namespace rendering { class Renderer; class LoadingScreen; }
namespace pipeline { class AssetManager; class DBCLayout; }

namespace core {


/// Handles terrain streaming, map transitions, world preloading,
/// and coordinate-aware tile management for online world entry.
///
/// Without a renderer - a client that draws nothing - a map has no geometry to
/// build: a load still goes through every stage, with nothing to read, upload
/// or stand on, and is ready when its warmup has run its course.
class WorldLoader {
    struct Load;
public:
    /// What the stages are timed by: the steady clock, or a test's own.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// `lastWorldFile` is where the last map entered is kept for the next
    /// start's preload; `clock` times the stages.
    WorldLoader(const Phase& phase, rendering::Renderer* renderer, pipeline::AssetManager* assetManager,
                std::filesystem::path lastWorldFile = defaultLastWorldFile(),
                Clock clock = &std::chrono::steady_clock::now);
    ~WorldLoader();

    /// The per-user last_world.cfg the client keeps.
    [[nodiscard]] static std::filesystem::path defaultLastWorldFile();

    struct Entry { uint32_t mapId; float x, y, z; };
    enum class Stage { Idle, Preparing, Prepared, Geometry, Collision, GeometryReady, Warmup, Finished, Failed };

    /// Lifecycle commands are issued by ClientLoop in Transitions.
    class Session {
    public:
        Session(WorldLoader& owner, std::unique_ptr<Load> load);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
    private:
        WorldLoader& owner_;
        std::unique_ptr<Load> load_;
    };
    std::unique_ptr<Session> begin(Entry entry, JobGroup& jobs);
    bool initializeScene();
    void beginWarmup();
    bool finish();
    /// Streaming phase: poll worker results and progress without pumping a frame.
    const WorldLoadStatus& update(float deltaTime);
    void observeReadiness(bool spawnsPending);
    const WorldLoadStatus& status() const;
    Stage stage() const;
    float warmupSeconds() const;
    Entry entry() const;
    const std::string& mapName() const;
    bool isInstance() const;

    // Map name utilities
    static const char* mapIdToName(uint32_t mapId);
    static const char* mapDisplayName(uint32_t mapId);

    // Background preloading - warms AssetManager file cache
    void startWorldPreload(uint32_t mapId, const std::string& mapName,
                           float serverX, float serverY);
    void cancelWorldPreload();

    // Persistent world info for session-to-session preloading
    void saveLastWorldInfo(uint32_t mapId, const std::string& mapName,
                           float serverX, float serverY);
    struct LastWorldInfo {
        uint32_t mapId = 0;
        std::string mapName;
        float x = 0, y = 0;
        bool valid = false;
    };
    [[nodiscard]] LastWorldInfo loadLastWorldInfo() const;

    // State accessors
    [[nodiscard]] uint32_t getLoadedMapId() const;
    [[nodiscard]] bool hasPendingEntry() const { return pendingWorldEntry_.has_value(); }

    // Get cached map name by ID (returns empty string if not found)
    [[nodiscard]] std::string getMapNameById(uint32_t mapId) const {
        auto it = mapNameById_.find(mapId);
        return (it != mapNameById_.end()) ? it->second : std::string{};
    }

    // ClientLoop takes the last pending destination at its transition boundary.
    void setPendingEntry(uint32_t mapId, float x, float y, float z) {
        pendingWorldEntry_ = Entry{.mapId = mapId, .x = x, .y = y, .z = z};
    }

    // Reset methods (for logout / character switch)
    void discardPendingEntry() { pendingWorldEntry_.reset(); }
    std::optional<Entry> takePendingEntry();
    void resetMapNameCache() { mapNameCacheLoaded_ = false; mapNameById_.clear(); }

private:
    struct PreparedMap;
    const Phase& phase_;
    rendering::Renderer* renderer_;
    pipeline::AssetManager* assetManager_;
    std::filesystem::path lastWorldFile_;
    bool reportedLastWorldFailure_ = false;
    Clock clock_;
    Load* load_ = nullptr;  // Borrowed from MapScope's Session.
    [[nodiscard]] float secondsSince(std::chrono::steady_clock::time_point start) const;
    void fail(const std::string& reason);
    std::string resolveMapName(uint32_t mapId);
    static std::shared_ptr<PreparedMap> prepareMap(pipeline::AssetManager& assets,
        const std::string& mapName, const JobGroup::Token& token);
    std::optional<Entry> pendingWorldEntry_;

    // Map.dbc name cache (loaded once per session)
    bool mapNameCacheLoaded_ = false;
    std::unordered_map<uint32_t, std::string> mapNameById_;

    // Background world preloader - warms AssetManager file cache for the
    // expected world before the user clicks Enter World.
    struct WorldPreload {
        uint32_t mapId = 0;
        std::string mapName;
        int centerTileX = 0;
        int centerTileY = 0;
        std::atomic<bool> cancel{false};
        std::vector<std::thread> workers;
    };
    std::unique_ptr<WorldPreload> worldPreload_;
};

} // namespace core
} // namespace wowee
