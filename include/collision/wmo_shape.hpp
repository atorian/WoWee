#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include "pipeline/wmo_bsp.hpp"

namespace wowee::pipeline { struct WMOGroup; }
namespace wowee::collision {
struct WmoLiquidShape {
    uint32_t xVerts=0,yVerts=0,xTiles=0,yTiles=0;
    glm::vec3 corner{};
    uint16_t liquidType=0;
    bool valid=true;
    std::vector<glm::vec3> vertices;
    std::vector<uint8_t> tiles;
};
struct WmoOriginalGroup {
    pipeline::WmoGroupLoadState loadState = pipeline::WmoGroupLoadState::NotLoaded;
    uint32_t groupId = 0;
    glm::vec3 boundsMin{},boundsMax{};
    std::shared_ptr<const pipeline::WmoBspData> bsp;
    std::vector<uint16_t> doodadReferences;
    std::optional<WmoLiquidShape> liquid;
};


/// What one group of a building (a WMO) is solid with: its triangles in the
/// building's model space, binned into a grid, with each triangle's normal,
/// height span and MOPY flags.
///
/// Built once per group - on the terrain worker, with the rest of the model's
/// preparation - and shared by every placement of the building and by the
/// renderer, which draws from the same triangles (its ray-traced mesh) and
/// culls by the same box. Immutable once built, so the queries may read it
/// from any thread.
struct WmoGroupShape {
    /// MOGP flags: whether the group is indoors, whether it has vertex colours.
    uint32_t groupFlags = 0;
    /// The box around the group, in model space.
    glm::vec3 boundingBoxMin{0.0f};
    glm::vec3 boundingBoxMax{0.0f};

    // Source bounds/topology/liquids, unaffected by visual bounds repair or
    // render-batch filtering. Absent for hand-built legacy query fixtures.
    std::optional<WmoOriginalGroup> original;
    std::vector<glm::vec3> vertices;
    std::vector<uint16_t> indices;
    /// MOCV per vertex, for an interior group that has them: the floor's
    /// colour is what lights a unit standing on it. Empty for every other
    /// group.
    std::vector<glm::vec3> vertexColors;

    static constexpr float kCellSize = 4.0f;
    int gridCellsX = 0;
    int gridCellsY = 0;
    glm::vec2 gridOrigin{0.0f};
    /// Per cell, the start index (into `indices`) of every triangle whose XY
    /// box reaches it, and the same split into floors and walls by
    /// kWallMaxAbsNormalZ.
    std::vector<std::vector<uint32_t>> cellTriangles;
    std::vector<std::vector<uint32_t>> cellFloorTriangles;
    std::vector<std::vector<uint32_t>> cellWallTriangles;

    struct TriBounds { float minZ; float maxZ; };
    /// Per triangle (start index / 3): its height span, its unit normal and
    /// its MOPY flags.
    std::vector<TriBounds> triBounds;
    std::vector<glm::vec3> triNormals;
    std::vector<uint8_t> triMopyFlags;
    /// True when no triangle in this group blocks: no collision hull, and
    /// nothing rendered that is not detail. Detail never blocks, so such a
    /// group would be walk-through in its entirety - which is a thing to be
    /// walked through only if it was meant to be, and Darkshore's bridges are
    /// 428 triangles of it. So for such a group detail counts.
    bool noBlockingTriangles = false;

    /// Where a building's surface stops being a floor and becomes a wall, as
    /// the absolute z of its normal: cos 49.46 degrees.
    ///
    /// The static sort into floor and wall cells uses it, and the wall pass
    /// skips anything at or above it because grounding handles those. A
    /// drifted pair would be a surface the player can neither walk on nor be
    /// stopped by. Not the slope-slide cutoff (cos 50 degrees, with the
    /// sliding code), and not M2's: that classifies floors at 0.35 so steep
    /// stairs stay walkable while still blocking, so its two cutoffs overlap
    /// where these do not.
    static constexpr float kWallMaxAbsNormalZ = 0.65f;

    /// A group's shape. `vertexColors` is empty or one per vertex, and
    /// `mopyFlags` empty or one per triangle. Says at warning when the group
    /// offers no floor at all.
    [[nodiscard]] static std::shared_ptr<const WmoGroupShape> build(
        uint32_t groupFlags, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
        std::vector<glm::vec3> vertices, std::vector<uint16_t> indices,
        std::vector<glm::vec3> vertexColors, std::vector<uint8_t> mopyFlags,
        std::optional<WmoOriginalGroup> original = {});
    [[nodiscard]] static std::shared_ptr<const WmoGroupShape> build(const pipeline::WMOGroup& group);

    /// The triangles of the grid a model-space XY box reaches, each once: any
    /// triangle, floors only, walls only.
    void trianglesInRange(float minX, float minY, float maxX, float maxY,
                          std::vector<uint32_t>& out) const;
    void floorTrianglesInRange(float minX, float minY, float maxX, float maxY,
                               std::vector<uint32_t>& out) const;
    void wallTrianglesInRange(float minX, float minY, float maxX, float maxY,
                              std::vector<uint32_t>& out) const;

    /// The triangles a model-space ray could hit inside the group: those
    /// between where it enters the group's box and where it leaves. False when
    /// it misses the box.
    ///
    /// Not those around the ray's origin. The two are the same place only
    /// while the ray is vertical in the group's own space, and a placement
    /// with pitch makes a vertical world ray lean - Darkshore's bridges, which
    /// were fallen through.
    bool trianglesAlongRay(const glm::vec3& localOrigin, const glm::vec3& localDir,
                           std::vector<uint32_t>& out) const;

    [[nodiscard]] uint64_t bytes() const;

private:
    void gatherCellTriangles(const std::vector<std::vector<uint32_t>>& cells,
                             float minX, float minY, float maxX, float maxY,
                             std::vector<uint32_t>& out) const;
};

/// What a building is solid with: its groups' shapes, in the order the
/// renderer keeps its groups, and what a query needs to know of the whole.
struct WmoShape {
    /// The renderer's id for the model, which is what the sound picks the
    /// ambience inside a building by.
    uint32_t modelId = 0;
    std::vector<std::shared_ptr<const WmoGroupShape>> groups;
    // Every authored group in file order, including groups with no render
    // batches. Shares shapes with groups above wherever they are rendered.
    std::vector<std::shared_ptr<const WmoGroupShape>> originalGroups;
    glm::vec3 boundingBoxMin{0.0f};
    glm::vec3 boundingBoxMax{0.0f};
    /// A flat model (a dock, a platform) whose floor is searched for further
    /// above and below its box than a building's.
    bool isLowPlatform = false;
    /// MOHD's ambient colour, which lights a unit inside.
    glm::vec3 ambientColor{0.5f};

    [[nodiscard]] uint64_t bytes() const;
};

}  // namespace wowee::collision
