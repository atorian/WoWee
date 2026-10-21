#pragma once

#include "collision/geometry.hpp"
#include "collision/m2_shape.hpp"
#include "collision/spatial_grid.hpp"
#include "collision/terrain_heights.hpp"
#include "collision/wmo_shape.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::collision {

/// The solid world of the map being shown: the ground of its loaded tiles, and
/// every building (WMO) and doodad (M2) placed in it with the shape it is
/// solid with. Answers where the floor is, what a mover slides along, what a
/// ray or a line of sight meets and whether a point is inside a building.
///
/// It used to be answered by the renderers, from their own instance lists and
/// GPU models, so everything that needed to know where the ground is - the
/// camera, the game's movers, transports, footprints - reached into the
/// renderer for it, and the renderer could not draw from data of its own on
/// another thread. The shapes are shared with the renderers, not copied; the
/// placements are this world's own.
///
/// Positions are in render space. Instance ids are the renderers': each
/// renderer places, moves and removes its instances here as it does in its
/// own list, so the two cannot disagree about what is in the world.
///
/// Threads: changed on the main thread only - at the frame's scene apply
/// point and in the terrain's tile commit and unload. Asked from the main
/// thread and from work the main thread starts and joins (the camera's floor
/// queries); never while a change is being made. The shapes it hands out
/// references into are immutable.
class CollisionWorld {
public:
    CollisionWorld();
    ~CollisionWorld();
    CollisionWorld(const CollisionWorld&) = delete;
    CollisionWorld& operator=(const CollisionWorld&) = delete;

    // ---- Doodads (M2) -----------------------------------------------------

    /// A doodad instance with a shape, placed by `transform` at `scale` (the
    /// placement's own scale; a scale of nothing collides with nothing).
    /// Placing an id again replaces it.
    void placeM2(uint32_t instanceId, std::shared_ptr<const M2Shape> shape,
                 const glm::mat4& transform, float scale);
    void moveM2(uint32_t instanceId, const glm::mat4& transform, float scale);
    void removeM2(uint32_t instanceId);
    /// Out of the world for now (a transport's deck doodads on the other
    /// continent): found by nothing until shown again.
    void setM2Hidden(uint32_t instanceId, bool hidden);
    /// Collides with nothing at all, or keeps its floors and stops nobody.
    void setM2SkipCollision(uint32_t instanceId, bool skip);
    void setM2SkipWalls(uint32_t instanceId, bool skip);
    void clearM2();

    // ---- Buildings (WMO) --------------------------------------------------

    void placeWmo(uint32_t instanceId, std::shared_ptr<const WmoShape> shape,
                  const glm::mat4& transform);
    void moveWmo(uint32_t instanceId, const glm::mat4& transform);
    void removeWmo(uint32_t instanceId);
    void setWmoHidden(uint32_t instanceId, bool hidden);
    /// A moving transport (ship hull, elevator). It keeps ordinary collision -
    /// a rider gets the deck from wmoInstanceFloor, but everyone else needs
    /// the hull solid to walk aboard - and the floor query takes its deck only
    /// when it is genuinely underfoot, so a lift cycling past a bystander
    /// cannot become their ground (the Undercity elevator yo-yo).
    void setWmoTransport(uint32_t instanceId, bool isTransport);
    void clearWmo();

    // ---- Terrain ----------------------------------------------------------

    void addTerrainTile(std::shared_ptr<const TerrainTileHeights> tile);
    void removeTerrainTile(int tileX, int tileY);
    void clearTerrain();

    /// Everything: the map is being left.
    void clear();

    // ---- The ground -------------------------------------------------------

    /// The terrain's surface under a point - the surface that is drawn, from
    /// the same sampler as the mesh. Nothing where no tile is loaded or the
    /// chunk has no heights. Holes are not cut: it interpolates straight
    /// across one and reports a surface that is not there - see terrainHoleAt.
    [[nodiscard]] std::optional<float> terrainHeight(float x, float y) const;
    /// Whether the single quad under a point is cut away - matched to the
    /// quads the mesh builder skips. The question for whether the player is
    /// standing on ground: a chunk with one cave mouth is mostly ground.
    [[nodiscard]] bool terrainHoleAt(float x, float y) const;
    /// Whether the 33-yard chunk under a point is cut by any hole: the
    /// heightfield is fiction somewhere in it, so anything reasoning about
    /// "below the terrain" there is a guess. Coarse in the safe direction, for
    /// what only ever gates a safety net.
    [[nodiscard]] bool terrainChunkHasHoles(float x, float y) const;
    /// Whether the tile under a point has finished streaming in. terrainHeight
    /// answers nothing both for a hole, which a character should fall through,
    /// and for a tile not here yet, which it should not: on a slow device the
    /// character enters the world before the ground does.
    [[nodiscard]] bool hasTerrainAt(float x, float y) const;
    /// The AreaTable id of the chunk under a point - the precise MCNK one,
    /// found by the same search as the height, so a zone border does not move
    /// as the player walks along it.
    [[nodiscard]] std::optional<uint32_t> areaIdAt(float x, float y) const;

    // ---- Buildings --------------------------------------------------------

    /// The building floor under (x, y) at or below z - which the caller has
    /// already raised by its step-up budget. Among those, the highest; or,
    /// given `referenceZ` (the true feet), the one nearest it, so a mover under
    /// an overhang is not pulled up to the level above. outNormalZ is the
    /// floor's |normal.z|, outFacesUp whether its face points up.
    [[nodiscard]] std::optional<float> wmoFloor(
        const Reach& reach, float x, float y, float z, float* outNormalZ = nullptr,
        float referenceZ = std::numeric_limits<float>::quiet_NaN(),
        bool* outFacesUp = nullptr) const;
    /// One building's floor, placed where `placement` says - its owner's pose
    /// for this frame, which may be ahead of the one this world was given.
    [[nodiscard]] std::optional<float> wmoInstanceFloor(uint32_t instanceId,
                                                       const glm::mat4& placement,
                                                       float x, float y, float z,
                                                       float* outNormalZ = nullptr) const;
    /// Whether a building instance is in this world with its shape.
    [[nodiscard]] bool hasWmo(uint32_t instanceId) const;
    /// A character-sized cylinder moving from `from` to `to`, pushed out of the
    /// building walls it meets; true when one pushed it. Indoors the radius is
    /// tighter and the pushes larger.
    bool wmoWallSlide(const Reach& reach, const glm::vec3& from, const glm::vec3& to,
                      glm::vec3& adjusted, bool insideWmo) const;
    /// Whether solid building geometry stands between two points: a line of
    /// sight, not a mover. Both ends are excluded by a small margin.
    [[nodiscard]] bool wmoSegmentBlocked(const Reach& reach, const glm::vec3& from,
                                         const glm::vec3& to) const;
    /// The distance along a ray to the first building surface it meets, up to
    /// maxDistance: the camera's collision with buildings.
    [[nodiscard]] float wmoRaycast(const Reach& reach, const glm::vec3& origin,
                                   const glm::vec3& direction, float maxDistance,
                                   bool cameraQuery = true) const;
    /// Whether a point is inside any group of a building, and which building
    /// model holds it.
    [[nodiscard]] bool insideWmo(const Reach& reach, float x, float y, float z,
                                 uint32_t* outModelId = nullptr) const;
    /// Whether a point is inside one of a building's interior groups - which
    /// is what decides whether the sky and the outdoor light are drawn.
    /// Stormwind's streets are exterior groups of one city-wide WMO.
    [[nodiscard]] bool insideInteriorWmo(const Reach& reach, float x, float y, float z) const;

    /// The light a unit standing at a point takes from the interior group that
    /// holds it: the building's ambient colour, and the vertex colour of the
    /// floor below the point when the group has them. Nothing outside every
    /// interior group. Anywhere in the world.
    struct InteriorSample {
        glm::vec3 ambient{0.5f};
        std::optional<glm::vec3> floorColor;
    };
    [[nodiscard]] std::optional<InteriorSample> interiorAt(float x, float y, float z) const;

    /// Changes whenever the buildings answering the queries do - one placed,
    /// removed or hidden. An answer kept from an earlier revision may be out
    /// of date.
    [[nodiscard]] uint64_t wmoRevision() const { return wmoRevision_; }

    // ---- Doodads ----------------------------------------------------------

    /// The doodad floor under (x, y) at or below z + 3, from collision meshes
    /// only: a bush or a sign is not there to stand on.
    [[nodiscard]] std::optional<float> m2Floor(const Reach& reach, float x, float y, float z,
                                               float* outNormalZ = nullptr) const;
    /// One doodad's floor at or under z, placed by `placement`.
    [[nodiscard]] std::optional<float> m2InstanceFloor(uint32_t instanceId,
                                                       const glm::mat4& placement,
                                                       float x, float y, float z) const;
    /// The world box of one doodad's collision mesh, placed by `placement`.
    /// False for an instance this world does not have - one with no shape.
    bool m2InstanceBounds(uint32_t instanceId, const glm::mat4& placement,
                          glm::vec3& outMin, glm::vec3& outMax) const;
    /// A cylinder of `radius` moving from `from` to `to`, pushed gently out of
    /// the doodad walls it meets; true when one pushed it.
    bool m2WallSlide(const Reach& reach, const glm::vec3& from, const glm::vec3& to,
                     glm::vec3& adjusted, float radius = 0.5f) const;
    /// The distance along a ray to the first doodad collision triangle, up to
    /// maxDistance: the camera's collision with doodads.
    [[nodiscard]] float m2Raycast(const Reach& reach, const glm::vec3& origin,
                                  const glm::vec3& direction, float maxDistance) const;

    // ---- Movers -----------------------------------------------------------

    /// Where a character-sized mover going straight from `from` to `to` ends
    /// up against building walls and - with `includeDoodads` - doodads: taken
    /// in steps short enough not to tunnel through a wall, and slid along
    /// whatever it meets. Indoors selects the tighter radius and finer steps.
    [[nodiscard]] glm::vec3 sweepMover(const Reach& reach, const glm::vec3& from,
                                       const glm::vec3& to, bool insideWmo,
                                       bool includeDoodads = true) const;

    // ---- Diagnostics ------------------------------------------------------

    /// Everything the floor queries would consider at a point, and what each
    /// contributes, at warning: what a fall-through report needs.
    void logFloorCandidates(float x, float y, float z) const;

    /// Time spent in queries and how many, since the last reset.
    struct QueryStats {
        double milliseconds = 0.0;
        uint32_t calls = 0;
    };
    [[nodiscard]] QueryStats wmoQueryStats() const;
    [[nodiscard]] QueryStats m2QueryStats() const;
    void resetQueryStats();

    /// What it holds: placements and terrain, and the shapes counted once.
    [[nodiscard]] uint64_t bytes() const;
    [[nodiscard]] size_t m2Count() const { return m2_.size(); }
    [[nodiscard]] size_t wmoCount() const { return wmo_.size(); }
    [[nodiscard]] size_t terrainTileCount() const { return terrain_.size(); }

private:
    struct M2Placement {
        uint32_t id = 0;
        std::shared_ptr<const M2Shape> shape;
        glm::mat4 transform{1.0f};
        glm::mat4 inverse{1.0f};
        float scale = 1.0f;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
        bool hidden = false;
        bool skipCollision = false;
        bool skipWalls = false;
    };
    struct WmoPlacement {
        uint32_t id = 0;
        std::shared_ptr<const WmoShape> shape;
        glm::mat4 transform{1.0f};
        glm::mat4 inverse{1.0f};
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
        /// Each group's world box, half a unit larger all round: a box fitted
        /// exactly rejects a query that starts on the surface, so a character
        /// standing on a floor could fail to find it.
        std::vector<std::pair<glm::vec3, glm::vec3>> groupBounds;
        bool hidden = false;
        bool isTransport = false;
    };
    struct GroupHolding {
        const WmoPlacement* placement;
        const WmoGroupShape* group;
        glm::vec3 localPos;
    };

    void boundM2(M2Placement& placement) const;
    void boundWmo(WmoPlacement& placement) const;
    void gatherM2(const glm::vec3& queryMin, const glm::vec3& queryMax,
                  std::vector<const M2Placement*>& out) const;
    void gatherWmo(const glm::vec3& queryMin, const glm::vec3& queryMax,
                   std::vector<const WmoPlacement*>& out) const;
    const TerrainTileHeights::Chunk* findChunkAt(float x, float y, float& fracX,
                                                 float& fracY) const;
    std::optional<GroupHolding> findGroupHolding(const Reach& reach, float x, float y, float z,
                                                 bool interiorOnly) const;

    std::unordered_map<uint32_t, M2Placement> m2_;
    SpatialGrid m2Grid_;
    std::unordered_map<uint32_t, WmoPlacement> wmo_;
    SpatialGrid wmoGrid_;
    uint64_t wmoRevision_ = 1;
    /// Tile key (x << 32 | y, as unsigned) to its ground.
    std::unordered_map<uint64_t, std::shared_ptr<const TerrainTileHeights>> terrain_;

    mutable std::atomic<double> wmoQueryMs_{0.0};
    mutable std::atomic<uint32_t> wmoQueryCalls_{0};
    mutable std::atomic<double> m2QueryMs_{0.0};
    mutable std::atomic<uint32_t> m2QueryCalls_{0};
};

}  // namespace wowee::collision
