#pragma once

#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"

#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <cstdint>

namespace wowee {
namespace pipeline {

/**
 * Vertex format for terrain rendering
 */
struct TerrainVertex {
    float position[3];     // X, Y, Z
    float normal[3];       // Normal vector
    float texCoord[2];     // Base texture coordinates
    float layerUV[2];      // Layer texture coordinates
    uint8_t chunkIndex = 0;    // Which chunk this vertex belongs to

    TerrainVertex()  {
        position[0] = position[1] = position[2] = 0.0f;
        normal[0] = normal[1] = normal[2] = 0.0f;
        texCoord[0] = texCoord[1] = 0.0f;
        layerUV[0] = layerUV[1] = 0.0f;
    }
};

/**
 * Triangle index (3 vertices)
 */
using TerrainIndex = uint32_t;

/// The levels of detail a chunk can be drawn at. Level 0 is the full mesh;
/// each level after it spans twice the cells of the one before (level 1
/// drops the quads' centre vertices).
inline constexpr int kTerrainLodLevels = 4;

/// The most vertices a chunk's mesh has: its 145 grid vertices and the nine
/// outer vertices of each edge again, lowered, for its skirts.
inline constexpr uint32_t kChunkMaxVertices = 145 + 4 * 9;

/// The most indices a chunk's mesh holds: level 0's four triangles a quad,
/// each coarser level's two a cell, and every level's skirts - a strip of two
/// triangles per segment of each of the four edges.
constexpr uint32_t terrainChunkMaxIndices() {
    uint32_t total = 8 * 8 * 4 * 3;
    for (int level = 0; level < kTerrainLodLevels; ++level) {
        const uint32_t cells = level == 0 ? 8u : 8u >> (level - 1);
        if (level > 0) total += cells * cells * 2 * 3;
        total += 4 * cells * 2 * 3;
    }
    return total;
}
inline constexpr uint32_t kChunkMaxIndices = terrainChunkMaxIndices();

/**
 * Renderable terrain mesh for a single map chunk
 */
struct ChunkMesh {
    std::vector<TerrainVertex> vertices;
    /// Level 0: four triangles fanned from each quad's centre vertex, the
    /// ground as the data describes it. Collision, shadows and ray tracing
    /// read this one.
    std::vector<TerrainIndex> indices;

    /// Each coarser level's surface, as a range of `lodIndices` (level 0's is
    /// `indices`, and its range here is empty). A coarser level whose count
    /// is 0 is not offered - its cells would have to cover only part of a
    /// hole.
    struct IndexRange {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::vector<TerrainIndex> lodIndices;
    std::array<IndexRange, kTerrainLodLevels> lods{};
    /// Each level's skirt along each edge (row 0, row 8, column 0, column 8),
    /// as ranges of `lodIndices`. A skirt is drawn only along an edge whose
    /// neighbour is drawn at another level: between two chunks at the same
    /// level the edges are the same line and nothing shows through.
    std::array<std::array<IndexRange, 4>, kTerrainLodLevels> skirts{};
    /// How far each level's surface strays up or down from the full mesh, at
    /// worst, in yards: 0 for level 0, and never less for a level than for
    /// the one before. What a level costs the picture, seen from a distance,
    /// is this projected to the screen (terrainLodFor).
    std::array<float, kTerrainLodLevels> lodError{};
    /// Where the skirt vertices start in `vertices`: each edge's nine outer
    /// vertices again, lowered by the skirt depth, edge after edge (north,
    /// south, west, east in grid terms: row 0, row 8, column 0, column 8).
    /// Every level hangs skirts from its edges, so that two neighbours drawn
    /// at different levels never let the sky show through their seam.
    uint32_t skirtFirstVertex = 0;

    /// The chunk's surface normals as a 17x17 RGBA8 image (rgb = normal *
    /// 0.5 + 0.5): the outer vertices on even texels, the inner ones on odd,
    /// the texels between the average of their neighbours. The ground is lit
    /// from this rather than from its vertices, so a coarse level keeps the
    /// light and shade of every vertex it no longer draws.
    static constexpr int kNormalGridSize = 17;
    std::vector<uint8_t> normalGrid;

    /// The levels the chunk offers, bit per level; level 0 always.
    [[nodiscard]] uint32_t offeredLods() const {
        uint32_t mask = 1u;
        for (int level = 1; level < kTerrainLodLevels; ++level)
            if (lods[static_cast<size_t>(level)].count > 0) mask |= 1u << level;
        return mask;
    }

    // Chunk position in world space
    float worldX;
    float worldY;
    float worldZ;

    // Chunk grid coordinates
    int chunkX;
    int chunkY;

    // Texture layer info
    struct LayerInfo {
        uint32_t textureId;
        uint32_t flags;
        std::vector<uint8_t> alphaData;  // 64x64 alpha map
    };
    std::vector<LayerInfo> layers;

    [[nodiscard]] bool isValid() const { return !vertices.empty() && !indices.empty(); }
    [[nodiscard]] size_t getVertexCount() const { return vertices.size(); }
    [[nodiscard]] size_t getTriangleCount() const { return indices.size() / 3; }
};

/// The level a chunk is drawn at from `distance` yards (its nearest point)
/// by a camera whose vertical focal length is `focalPixels`: the coarsest
/// offered level whose error comes out no larger than `maxErrorPixels` on
/// screen. Flat ground drops to its coarsest level close in; a mountain
/// keeps its detail as far as its shape can be seen.
[[nodiscard]] int terrainLodFor(const std::array<float, kTerrainLodLevels>& errorYards,
                                uint32_t offeredLods, float distance, float focalPixels,
                                float maxErrorPixels);

/**
 * Complete terrain tile mesh (16x16 chunks)
 */
struct TerrainMesh {
    std::array<ChunkMesh, 256> chunks;  // 16x16 grid
    std::vector<std::string> textures;   // Texture filenames

    int validChunkCount = 0;

    [[nodiscard]] const ChunkMesh& getChunk(int x, int y) const { return chunks[y * 16 + x]; }
    ChunkMesh& getChunk(int x, int y) { return chunks[y * 16 + x]; }
};

/// The heap memory the mesh holds (pipeline/heap_bytes.hpp).
[[nodiscard]] uint64_t heapBytes(const TerrainMesh& mesh);

/**
 * Terrain mesh generator
 *
 * Converts ADT heightmap data into renderable triangle meshes
 */
class TerrainMeshGenerator {
public:
    /**
     * Generate terrain mesh from ADT data
     * @param terrain Loaded ADT terrain data
     * @return Generated mesh (check validChunkCount)
     */
    static TerrainMesh generate(const ADTTerrain& terrain);

    /**
     * Where a point inside a chunk sits in the world.
     *
     * `fracX` and `fracY` are cell coordinates within the chunk, 0 to 8, and
     * need not be whole: the height between the four surrounding grid points
     * is interpolated bilinearly. This is what the ground-clutter and doodad
     * scatterers ask when they drop something at a random spot on a chunk,
     * and both had their own copy of it.
     *
     * The axes cross on purpose. A chunk's world X runs against the grid's
     * Y and its world Y against the grid's X, which is the terrain axis
     * pairing this codebase uses throughout; swapping them back lays every
     * scattered object out mirrored, which reads as the doodad data being
     * wrong rather than the sampling.
     *
     * Out-of-range coordinates clamp to the chunk rather than reading past
     * it, so a caller that rounds slightly past 8 gets the edge height
     * instead of a zero that would bury the object.
     */
    static glm::vec3 chunkSurfacePoint(const float chunkPosition[3],
                                       const HeightMap& heightMap,
                                       float fracX, float fracY, float unitSize);

    /**
     * Where a world position falls inside a chunk, in the 0..8 grid fractions
     * chunkSurfacePoint and isHole take. False if the position is outside it.
     *
     * The axes cross here too: world X gives fracY and world Y gives fracX.
     *
     * Separated so a caller walking many points can test the chunk it already
     * has before searching for another. The search is a tile lookup and a 3x3
     * probe, and running it per sample dominated grass generation - a hundred
     * thousand candidates in a row nearly all land in the chunk the last one
     * did.
     */
    static bool chunkFractionsAt(const float chunkPosition[3], float glX, float glY,
                                 float unitSize, float& fracX, float& fracY);

private:
    /**
     * Generate mesh for a single map chunk
     */
    static ChunkMesh generateChunkMesh(const MapChunk& chunk, int chunkX, int chunkY, int tileX, int tileY);

    /**
     * Generate vertices from heightmap
     * WoW heightmap layout: 9x9 outer + 8x8 inner vertices (145 total)
     */
    static std::vector<TerrainVertex> generateVertices(const MapChunk& chunk, int chunkX, int chunkY, int tileX, int tileY);

    /**
     * Generate triangle indices
     * Creates triangles that connect the heightmap vertices
     * Skips quads that are marked as holes in the chunk
     */
    static std::vector<TerrainIndex> generateIndices(const MapChunk& chunk);

    /**
     * The coarser levels of detail, every level's skirts and each level's
     * error: appends the skirt vertices to `mesh.vertices` and fills
     * `mesh.lodIndices`, `mesh.lods` and `mesh.lodError`.
     */
    static void generateLods(const MapChunk& chunk, ChunkMesh& mesh);

    /// The 17x17 normal image from the 145 vertices' normals.
    static std::vector<uint8_t> generateNormalGrid(const std::vector<TerrainVertex>& vertices);


    /**
     * Convert WoW's compressed normals to float
     */
    static void decompressNormal(const int8_t* compressedNormal, float* normal);

    /**
     * Get height at grid position from WoW's 9x9+8x8 layout
     */
    static float getHeightAt(const HeightMap& heightMap, int x, int y);



    // Terrain constants
    // WoW terrain: 64x64 tiles, each tile = 533.33 yards, each chunk = 33.33 yards
    // One ADT tile, from the coordinate header rather than spelled again:
    // the value is a truncation of 1600/3, and a second spelling of a
    // truncation is a second answer to where a tile boundary is.
    static constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    static constexpr float CHUNK_SIZE = TILE_SIZE / 16.0f; // One chunk = 33.33 yards (16 chunks per tile)
    static constexpr float GRID_STEP = CHUNK_SIZE / 8.0f;  // 8 quads per chunk = 4.17 yards per vertex
};

} // namespace pipeline
} // namespace wowee
