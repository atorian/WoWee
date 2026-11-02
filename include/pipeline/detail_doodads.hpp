#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "pipeline/adt_loader.hpp"

// Ground clutter - the grass, flowers, stones and bones on the terrain - laid
// out the way the 3.3.5a client lays it out (WoW.exe 12340,
// CMapChunk::CreateDetailDoodads at 0x7d3390).
//
// The client draws `groundEffectDensity` random quads of the chunk's 8x8 grid,
// repeats allowed. A quad the map marks with no effect doodads, or a hole, grows
// nothing; otherwise the quad's ground effect comes from the texture layer its
// low-quality texture map names, and that effect's GroundEffectTexture row
// places `density` doodads (8 when the row says 0) at random spots in the quad,
// each one of its four doodads by weight. A doodad on ground steeper than a
// normal of 0.4 up is dropped. Every doodad turns a random amount about the up
// axis and scales by 1 +- 0.33.
//
// All of it is drawn from the client's own generator, seeded per chunk with
// the chunk's position in the world, so it comes out where the client puts it.

namespace wowee {
namespace pipeline {

/// One GroundEffectTexture row: what a texture layer grows.
struct GroundEffectTexture {
    std::array<uint32_t, 4> doodadIds{};
    std::array<uint32_t, 4> weights{};
    /// Doodads per drawn quad; 0 means the client's default of 8.
    uint32_t density = 0;
};

/// One GroundEffectDoodad row: a model that grows.
struct GroundEffectDoodad {
    std::string modelPath;
    uint32_t flags = 0;
    /// Flat clutter - bones, leaves, clover - lies along the ground's slope
    /// instead of standing upright.
    [[nodiscard]] bool liesOnGround() const { return (flags & 1u) != 0; }
};

struct GroundEffectTables {
    std::unordered_map<uint32_t, GroundEffectTexture> textures;  // by effect id
    std::unordered_map<uint32_t, GroundEffectDoodad> doodads;    // by doodad id
};

/// The client's groundEffectDensity: how many quads of each chunk are drawn.
/// Its default and range (0x78e780; the setter refuses values outside it).
inline constexpr uint32_t kDefaultGroundEffectDensity = 16;
inline constexpr uint32_t kMinGroundEffectDensity = 16;
inline constexpr uint32_t kMaxGroundEffectDensity = 256;

/// The client's groundEffectDist, yards: a chunk whose nearest point is this
/// far ahead of the camera or more has no clutter, and the clutter fades out
/// over the last 15% of it (0x7b1ac6).
inline constexpr float kDefaultGroundEffectDist = 70.0f;
inline constexpr float kMaxGroundEffectDist = 140.0f;
inline constexpr float kGroundEffectFadeStart = 0.85f;

struct DetailDoodadPlacement {
    uint32_t doodadId = 0;
    glm::vec3 position{0.0f};  // on the terrain, render space
    glm::vec3 groundNormal{0.0f, 0.0f, 1.0f};  // of the terrain triangle under it
    float rotation = 0.0f;  // radians about up, 0..2pi
    float scale = 1.0f;
};

struct DetailDoodadLayout {
    std::vector<DetailDoodadPlacement> placements;
    /// Ids the chunk asked for that the tables do not have, for the caller to
    /// report: the client skips such a quad or doodad and so does this.
    std::vector<uint32_t> unknownEffects;
    std::vector<uint32_t> unknownDoodads;
    /// Quads whose low-quality texture map names a layer the chunk lacks.
    uint32_t missingLayers = 0;
};

struct M2Model;

/// What the client draws of a detail doodad: the model's mesh as it stands in
/// its file - no bones, no animation - and the texture of each batch
/// (0x7b1b50 reads the skin's vertices straight).
struct DetailDoodadModel {
    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec2 uv{0.0f};
    };
    struct Batch {
        std::string texture;  // the file, as the model names it
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
    };
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<Batch> batches;
};

/// The model's base level of detail and its file textures. Batches whose
/// texture is not a file (a replaceable slot) have none to draw with and are
/// left out; `skippedBatches` counts them for the caller to report.
[[nodiscard]] DetailDoodadModel detailDoodadModelFrom(const M2Model& model,
                                                      uint32_t* skippedBatches = nullptr);

/// One chunk's clutter as the GPU draws it: every placed doodad's mesh moved
/// to where it stands, in one vertex and index list, a batch per texture.
struct DetailChunkMesh {
    /// 32 bytes; matches the vertex input of detail_doodad.vert.glsl.
    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};  // the ground's: clutter is lit as the ground
        glm::vec2 uv{0.0f};
    };
    struct Batch {
        std::string texture;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
    };
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

/// The doodad's mesh moved onto its spot: turned by `rotation` about up,
/// scaled, and - for clutter that lies on the ground - tilted from up onto the
/// ground's normal (0x7b1000).
[[nodiscard]] glm::mat3 detailDoodadBasis(const DetailDoodadPlacement& placement,
                                          bool liesOnGround);

/// Lay `placements` out as one mesh. `modelFor` gives the model of a doodad
/// id, or null for one that has none; its placements are left out.
[[nodiscard]] DetailChunkMesh buildDetailChunkMesh(
    const std::vector<DetailDoodadPlacement>& placements, const GroundEffectTables& tables,
    const std::function<const DetailDoodadModel*(uint32_t doodadId)>& modelFor);

/// The client's seed for a chunk: its row and column in the whole map.
[[nodiscard]] uint32_t detailDoodadSeed(uint32_t globalChunkX, uint32_t globalChunkY);

/// Lay out one chunk's ground clutter. `globalChunkX/Y` are the chunk's
/// column and row in the map (tile * 16 + chunk in tile); `quadDraws` is
/// groundEffectDensity.
[[nodiscard]] DetailDoodadLayout placeDetailDoodads(const MapChunk& chunk,
                                                    uint32_t globalChunkX, uint32_t globalChunkY,
                                                    const GroundEffectTables& tables,
                                                    uint32_t quadDraws);

} // namespace pipeline
} // namespace wowee
