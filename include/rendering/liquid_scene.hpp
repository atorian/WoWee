#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace wowee {
namespace pipeline {
struct ADTTerrain;
struct WMOLiquid;
}

namespace rendering {

/// One sheet of liquid as the map data lays it down: a chunk's layer of a
/// terrain tile, a tile's worth of layers merged at one height, or a WMO's
/// pool. A grid of `width` x `height` cells from `origin` along the two step
/// vectors, the height at each of its corners, and which cells hold liquid.
using LiquidSurfaceId = uint32_t;

/// A connected stretch of one kind of liquid: a pond, a river, the sea off a
/// coast. Named after the oldest patch of water in it, so it keeps its id for
/// as long as that patch is loaded, however the rest of it streams in and out.
using LiquidBodyId = uint64_t;

struct LiquidSurface {
    LiquidSurfaceId id = 0;

    glm::vec3 origin{0.0f};
    glm::vec3 stepX{0.0f};
    glm::vec3 stepY{0.0f};
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    uint16_t liquidType = 0;

    /// The terrain tile it came with, or -1 for a WMO's liquid.
    int tileX = -1;
    int tileY = -1;
    /// The WMO instance it belongs to; 0 for terrain liquid.
    uint32_t wmoId = 0;

    uint8_t xOffset = 0;
    uint8_t yOffset = 0;
    uint8_t width = 8;
    uint8_t height = 8;

    /// (width + 1) x (height + 1) corner heights.
    std::vector<float> heights;
    /// Which cells hold liquid; see waterCellRendered for its two shapes.
    std::vector<uint8_t> mask;

    /// 0 inland water, 1 ocean, 2 magma, 3 slime.
    [[nodiscard]] uint8_t basicType() const;
    [[nodiscard]] bool cellHasLiquid(int x, int y) const;
};

/// What a body of liquid is: what it is made of and how big it is.
struct LiquidBody {
    LiquidBodyId id = 0;
    /// 0 inland water, 1 ocean, 2 magma, 3 slime.
    uint8_t basicType = 0;
    /// Square yards of liquid, over every loaded surface it spans.
    float area = 0.0f;
    glm::vec2 boundsMin{0.0f};
    glm::vec2 boundsMax{0.0f};
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    /// Surfaces it spans; one surface can hold several bodies.
    uint32_t surfaceCount = 0;
    /// Some of it lies in a building rather than on the terrain.
    bool inWmo = false;
};

/// Told when liquid comes and goes, to keep what is drawn of it in step.
class LiquidSceneListener {
public:
    virtual ~LiquidSceneListener() = default;
    virtual void liquidSurfaceAdded(const LiquidSurface& surface) = 0;
    virtual void liquidSurfaceRemoved(LiquidSurfaceId id) = 0;
};

/// The liquid of the map being shown: where it is, how high, of what kind and
/// in which body. It owns no GPU resources - the water renderer draws what it
/// is told about here, and everything else that needs the water (the camera,
/// swimming, footsteps, the underwater overlay, grass) asks this instead of
/// the renderer.
///
/// Bodies are found where the surfaces are built rather than per query. Each
/// surface is split into patches, its own connected cells; a patch is joined
/// to every patch of another surface that its edge cells border, when both
/// are the same kind of liquid at much the same height. A body is a connected
/// set of patches. Height matters so that a lake over a flooded cave stays two
/// bodies, and so that a waterfall separates the pools above and below it.
class LiquidScene {
public:
    /// How far apart two touching cells may be in height and still be one
    /// body, in yards: a river's slope, not a waterfall.
    static constexpr float kMaxJoinStep = 2.0f;

    LiquidScene();
    ~LiquidScene();
    LiquidScene(const LiquidScene&) = delete;
    LiquidScene& operator=(const LiquidScene&) = delete;

    /// Ends a listener's subscription when destroyed.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(LiquidScene* scene, LiquidSceneListener* listener)
            : scene_(scene), listener_(listener) {}
        ~Subscription() { reset(); }
        Subscription(Subscription&& o) noexcept : scene_(o.scene_), listener_(o.listener_) {
            o.scene_ = nullptr;
            o.listener_ = nullptr;
        }
        Subscription& operator=(Subscription&& o) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        void reset();
    private:
        LiquidScene* scene_ = nullptr;
        LiquidSceneListener* listener_ = nullptr;
    };

    /// The listener is told of every surface already here before this returns.
    [[nodiscard]] Subscription subscribe(LiquidSceneListener& listener);

    /// A terrain tile's liquid. Returns how many surfaces it added.
    std::size_t addTerrainTile(const pipeline::ADTTerrain& terrain, int tileX, int tileY);
    /// A WMO group's liquid, placed by the instance's model matrix. False when
    /// it has none or cannot be placed; the latter is logged once per instance.
    bool addWmoLiquid(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix,
                      uint32_t wmoId);
    void removeTile(int tileX, int tileY);
    void removeWmo(uint32_t wmoId);
    void clear();

    [[nodiscard]] std::size_t surfaceCount() const { return surfaces_.size(); }
    [[nodiscard]] bool empty() const { return surfaces_.empty(); }
    [[nodiscard]] const std::vector<LiquidSurface>& surfaces() const { return surfaces_; }

    /// The highest liquid surface over a point.
    [[nodiscard]] std::optional<float> heightAt(float x, float y) const;
    /// The liquid surface over a point nearest `z`, among those from 2 yards
    /// below it to `maxAbove` above it - so a pool on a roof far overhead is
    /// not taken for the water the camera is in.
    [[nodiscard]] std::optional<float> nearestHeightAt(float x, float y, float z,
                                                       float maxAbove = 15.0f) const;
    /// The liquid type of the highest surface over a point.
    [[nodiscard]] std::optional<uint16_t> typeAt(float x, float y) const;
    /// Whether a point falls inside a WMO's liquid grid.
    [[nodiscard]] bool isWmoLiquidAt(float x, float y) const;
    /// The height of the water (not magma or slime) whose surface centre is
    /// nearest the camera, which the planar reflection mirrors about.
    [[nodiscard]] std::optional<float> dominantWaterHeight(const glm::vec3& cameraPos) const;

    /// The body the highest liquid over a point belongs to.
    [[nodiscard]] std::optional<LiquidBody> bodyAt(float x, float y) const;
    /// Every body of the liquid loaded now.
    [[nodiscard]] std::vector<LiquidBody> bodies() const;

private:
    struct Patch {
        LiquidSurfaceId surface = 0;
        float area = 0.0f;
        glm::vec2 boundsMin{0.0f};
        glm::vec2 boundsMax{0.0f};
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
        std::unordered_set<uint64_t> neighbours;
    };
    /// Per surface: which patch each cell is in, -1 for no liquid.
    struct SurfacePatches {
        std::vector<int32_t> cellPatch;
        uint32_t patchCount = 0;
    };

    static uint64_t patchKey(LiquidSurfaceId surface, uint32_t patch) {
        return (static_cast<uint64_t>(surface) << 32) | patch;
    }

    void addSurface(LiquidSurface&& surface);
    void buildPatches(const LiquidSurface& surface);
    void joinPatches(const LiquidSurface& surface);
    void dropPatches(LiquidSurfaceId id);
    template <class Pred> void removeSurfacesIf(Pred pred);
    void findBodies() const;
    /// The surface and cell under a point, highest surface first.
    struct CellHit {
        const LiquidSurface* surface = nullptr;
        int cellX = 0;
        int cellY = 0;
    };
    [[nodiscard]] std::optional<CellHit> highestCellAt(float x, float y) const;
    [[nodiscard]] const LiquidSurface* find(LiquidSurfaceId id) const;

    std::vector<LiquidSurface> surfaces_;
    LiquidSurfaceId nextId_ = 1;
    std::vector<LiquidSceneListener*> listeners_;

    std::unordered_map<LiquidSurfaceId, SurfacePatches> surfacePatches_;
    std::unordered_map<uint64_t, Patch> patches_;

    mutable bool bodiesDirty_ = true;
    mutable std::unordered_map<uint64_t, LiquidBodyId> bodyOfPatch_;
    mutable std::unordered_map<LiquidBodyId, LiquidBody> bodies_;

    std::unordered_set<uint32_t> rejectedWmoSaid_;
};

} // namespace rendering
} // namespace wowee
