#include "pipeline/terrain_mesh.hpp"
#include "pipeline/adt_alpha.hpp"
#include <algorithm>
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <cmath>

namespace wowee {
namespace pipeline {

TerrainMesh TerrainMeshGenerator::generate(const ADTTerrain& terrain) {
    TerrainMesh mesh;

    if (!terrain.isLoaded()) {
        LOG_WARNING("Attempting to generate mesh from unloaded terrain");
        return mesh;
    }


    // Copy texture list
    mesh.textures = terrain.textures;

    // Generate mesh for each chunk
    int validCount = 0;
    bool loggedFirstChunk = false;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            const MapChunk& chunk = terrain.getChunk(x, y);

            if (chunk.hasHeightMap()) {
                mesh.getChunk(x, y) = generateChunkMesh(chunk, x, y, terrain.coord.x, terrain.coord.y);
                validCount++;

                // Debug: log first chunk world position
                if (!loggedFirstChunk) {
                    loggedFirstChunk = true;
                    LOG_DEBUG("First terrain chunk world pos: (", chunk.position[0], ", ",
                              chunk.position[1], ", ", chunk.position[2], ")");
                }
            }
        }
    }

    mesh.validChunkCount = validCount;


    return mesh;
}

ChunkMesh TerrainMeshGenerator::generateChunkMesh(const MapChunk& chunk, int chunkX, int chunkY, int tileX, int tileY) {
    ChunkMesh mesh;

    mesh.chunkX = chunkX;
    mesh.chunkY = chunkY;

    // Compute render-space XY from tile/chunk indices (MCNK position fields are unreliable).
    // tileX increases southward (renderY axis), tileY increases eastward (renderX axis).
    // NW corner of tile: renderX = (32-tileY)*TILE_SIZE, renderY = (32-tileX)*TILE_SIZE
    // Each chunk step goes east (–renderX) or south (–renderY).
    const float tileNW_renderX = (32.0f - static_cast<float>(tileY)) * core::coords::TILE_SIZE;
    const float tileNW_renderY = (32.0f - static_cast<float>(tileX)) * core::coords::TILE_SIZE;
    mesh.worldX = tileNW_renderX - static_cast<float>(chunkY) * CHUNK_SIZE;  // iy controls renderX (east-west)
    mesh.worldY = tileNW_renderY - static_cast<float>(chunkX) * CHUNK_SIZE;  // ix controls renderY (north-south)
    mesh.worldZ = chunk.position[2];  // height base (wowZ) from MCNK offset 112

    // Debug: log chunk positions for first tile
    static int posLogCount = 0;
    if (posLogCount < 5) {
        posLogCount++;
        LOG_INFO("Terrain chunk: tile(", tileX, ",", tileY, ") ix=", chunkX, " iy=", chunkY,
                 " worldXY=(", mesh.worldX, ",", mesh.worldY, ",", mesh.worldZ, ")",
                 " mcnk=(", chunk.position[0], ",", chunk.position[1], ",", chunk.position[2], ")");
    }

    // Generate vertices from heightmap (pass chunk grid indices and tile coords)
    mesh.vertices = generateVertices(chunk, chunkX, chunkY, tileX, tileY);

    // Generate triangle indices (checks for holes)
    mesh.indices = generateIndices(chunk);
    // The far levels and their skirts, after the full mesh.
    generateLods(chunk, mesh);

    // Debug: verify mesh integrity (one-time)
    static bool debugLogged = false;
    if (!debugLogged && chunkX == 0 && chunkY == 0) {
        debugLogged = true;
        LOG_INFO("Terrain mesh debug: ", mesh.vertices.size(), " vertices, ",
                 mesh.indices.size(), " indices (", mesh.indices.size() / 3, " triangles)");

        // Verify all indices are in bounds
        int maxIndex = 0;
        int minIndex = 9999;
        for (auto idx : mesh.indices) {
            if (static_cast<int>(idx) > maxIndex) maxIndex = idx;
            if (static_cast<int>(idx) < minIndex) minIndex = idx;
        }
        LOG_INFO("Index range: [", minIndex, ", ", maxIndex, "] (expected [0, 144])");

        if (maxIndex >= static_cast<int>(mesh.vertices.size())) {
            LOG_ERROR("INDEX OUT OF BOUNDS! Max index ", maxIndex, " >= vertex count ", mesh.vertices.size());
        }

        // Check for invalid vertex positions
        int invalidCount = 0;
        for (const auto& v : mesh.vertices) {
            if (!std::isfinite(v.position[0]) || !std::isfinite(v.position[1]) || !std::isfinite(v.position[2])) {
                invalidCount++;
            }
        }
        if (invalidCount > 0) {
            LOG_ERROR("Found ", invalidCount, " vertices with invalid positions!");
        }
    }

    // Copy texture layers
    for (size_t layerIdx = 0; layerIdx < chunk.layers.size(); layerIdx++) {
        const auto& layer = chunk.layers[layerIdx];
        ChunkMesh::LayerInfo layerInfo;
        layerInfo.textureId = layer.textureId;
        layerInfo.flags = layer.flags;

        // Upload absolute coverage for both big-alpha and legacy maps.
        // Vegetation and dominant-texture queries use the same decoder.
        if (layer.useAlpha()) {
            std::vector<uint8_t> decoded;
            if (decodeLayerWeight(chunk, layerIdx, decoded, 0)) {
                layerInfo.alphaData = std::move(decoded);
            }
        }

        mesh.layers.push_back(layerInfo);
    }

    return mesh;
}

std::vector<TerrainVertex> TerrainMeshGenerator::generateVertices(const MapChunk& chunk, int chunkX, int chunkY, int tileX, int tileY) {
    std::vector<TerrainVertex> vertices;
    vertices.reserve(145);  // 145 vertices total

    const HeightMap& heightMap = chunk.heightMap;

    // WoW terrain uses 145 heights stored in a 9x17 row-major grid layout.
    //
    // Vertex XY is derived as a SINGLE multiply from the tile index:
    //   pos = TILE_SIZE * (32 - tile - gridStep/128)
    // where gridStep = chunk*8 + vertexOffset runs 0..128 across the 16 chunks of a
    // tile. The previous form subtracted the chunk base and the per-vertex step
    // separately, so a tile's far edge (…*C - C) and the neighbouring tile's near edge
    // ((…-1)*C) rounded to slightly different float32 values - a sub-yard gap that
    // opened hairline "blue" T-junction cracks between tiles, worst far from the map
    // origin (across Kalimdor). Collapsing both to one multiply makes the shared edge
    // bit-identical on either side, closing the seam without the overlap hacks.
    const float TS = core::coords::TILE_SIZE;
    constexpr float kStepsPerTile = 128.0f;  // 16 chunks * 8 vertex steps
    const float chunkBaseZ = chunk.position[2];  // height base (wowZ) from MCNK offset 112

    for (int index = 0; index < 145; index++) {
        int y = index / 17;  // Row (0-8)
        int x = index % 17;  // Column (0-16)

        // Columns 9-16 are offset by 0.5 units (wowee exact logic)
        float offsetX = static_cast<float>(x);
        float offsetY = static_cast<float>(y);

        if (x > 8) {
            offsetY += 0.5f;
            offsetX -= 8.5f;
        }

        TerrainVertex vertex;

        // Position in render space:
        //   MCVT rows (offsetY) go west→east = renderX decreasing
        //   MCVT columns (offsetX) go north→south = renderY decreasing
        // NaN heights are clamped - WHM load scrubs but mid-edit terrain
        // can briefly carry NaN before stitchEdges runs, and a single NaN
        // vertex would propagate into normal computations and crash culling.
        float h = heightMap.heights[index];
        if (!std::isfinite(h)) h = 0.0f;
        // Fractional grid position within the tile along each render axis (0..128).
        const float gridX = static_cast<float>(chunkY) * 8.0f + offsetY;  // row → renderX (west→east)
        const float gridY = static_cast<float>(chunkX) * 8.0f + offsetX;  // col → renderY (north→south)
        vertex.position[0] = (32.0f - static_cast<float>(tileY) - gridX / kStepsPerTile) * TS;  // renderX
        vertex.position[1] = (32.0f - static_cast<float>(tileX) - gridY / kStepsPerTile) * TS;  // renderY
        vertex.position[2] = chunkBaseZ + h;                                                     // renderZ

        // Normal
        if (index * 3 + 2 < static_cast<int>(chunk.normals.size())) {
            decompressNormal(&chunk.normals[index * 3], vertex.normal);
        } else {
            // Default up normal
            vertex.normal[0] = 0.0f;
            vertex.normal[1] = 0.0f;
            vertex.normal[2] = 1.0f;
        }

        // Texture coordinates: world-aligned so patterns don't reset per chunk.
        // Tile each texture 4× per chunk (one repeat every ~8 yards) so the
        // texture's own pattern noise breaks up the chunk grid rather than
        // syncing with it. At 1 repeat/chunk the per-chunk alpha differences
        // read as obvious 33-yard squares; at 4× the pattern is small enough
        // that the eye no longer locks onto the chunk boundary.
        constexpr float texScale = 4.0f / CHUNK_SIZE;
        vertex.texCoord[0] = -vertex.position[1] * texScale;
        vertex.texCoord[1] = -vertex.position[0] * texScale;

        // Layer UV for alpha map sampling (0-1 range per chunk).
        // Sample at texel centers of the 64x64 alpha map to avoid edge seams.
        constexpr float alphaTexels = 64.0f;
        constexpr float alphaStep = (alphaTexels - 1.0f) / 8.0f; // 63 texels across 8 quads
        vertex.layerUV[0] = (offsetX * alphaStep + 0.5f) / alphaTexels;
        vertex.layerUV[1] = (offsetY * alphaStep + 0.5f) / alphaTexels;

        vertices.push_back(vertex);
    }

    return vertices;
}

std::vector<TerrainIndex> TerrainMeshGenerator::generateIndices(const MapChunk& chunk) {
    std::vector<TerrainIndex> indices;
    indices.reserve(768);  // 8x8 quads * 4 triangles * 3 indices = 768

    // Generate indices based on 9x17 grid layout (matching wowee.js)
    // Each quad uses a center vertex with 4 surrounding vertices
    // Index offsets from center: -9, -8, +9, +8

    int holesSkipped = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            // Skip quads that are marked as holes (cave entrances, etc.)
            if (chunk.isHole(y, x)) {
                holesSkipped++;
                continue;
            }

            // Center vertex index in the 9x17 grid
            int center = 9 + y * 17 + x;

            // Four triangles per quad
            // Using CCW winding when viewed from +Z (top-down)
            int tl = center - 9;  // top-left outer
            int tr = center - 8;  // top-right outer
            int bl = center + 8;  // bottom-left outer
            int br = center + 9;  // bottom-right outer

            // Triangle 1: top (center, tl, tr)
            indices.push_back(center);
            indices.push_back(tl);
            indices.push_back(tr);

            // Triangle 2: right (center, tr, br)
            indices.push_back(center);
            indices.push_back(tr);
            indices.push_back(br);

            // Triangle 3: bottom (center, br, bl)
            indices.push_back(center);
            indices.push_back(br);
            indices.push_back(bl);

            // Triangle 4: left (center, bl, tl)
            indices.push_back(center);
            indices.push_back(bl);
            indices.push_back(tl);
        }
    }

    // Debug: log if any holes were skipped (one-time per session)
    static bool holesLogged = false;
    if (!holesLogged && holesSkipped > 0) {
        holesLogged = true;
        LOG_INFO("Terrain holes: skipped ", holesSkipped, " quads due to hole mask (holes=0x",
                 std::hex, chunk.holes, std::dec, ")");
    }

    return indices;
}

namespace {

/// Outer vertex (row r, column c, both 0-8) of the interleaved 9x17 layout.
constexpr int outerVertex(int r, int c) { return r * 17 + c; }

/// The i-th outer vertex (0-8) along one of the chunk's edges: row 0, row 8,
/// column 0, column 8.
constexpr int edgeVertex(int edge, int i) {
    switch (edge) {
        case 0: return outerVertex(0, i);
        case 1: return outerVertex(8, i);
        case 2: return outerVertex(i, 0);
        default: return outerVertex(i, 8);
    }
}

/// Whether the quad beside cell i (0-7) of an edge is a hole.
bool edgeCellIsHole(const MapChunk& chunk, int edge, int i) {
    switch (edge) {
        case 0: return chunk.isHole(0, i);
        case 1: return chunk.isHole(7, i);
        case 2: return chunk.isHole(i, 0);
        default: return chunk.isHole(i, 7);
    }
}

/// How far below a coarse level's edge its skirt reaches past the furthest
/// any level's edge strays from another's, in yards.
constexpr float kSkirtMargin = 1.0f;

}  // namespace

void TerrainMeshGenerator::generateLods(const MapChunk& chunk, ChunkMesh& mesh) {
    const auto height = [&](int vertex) { return mesh.vertices[static_cast<size_t>(vertex)].position[2]; };

    // Two chunks at neighbouring levels meet along an edge one draws through
    // all nine outer vertices and the other through every second or fourth:
    // between the shared vertices the two edges are different lines, and the
    // sky shows through the gap. Each coarse level hangs a skirt from its
    // edges, deep enough to pass below the furthest any level's edge can
    // stray from another's, so whichever side is higher, the lower one's
    // skirt fills the gap below it.
    float deviation = 0.0f;
    for (int edge = 0; edge < 4; ++edge) {
        for (int level = 2; level < kTerrainLodLevels; ++level) {
            const int stride = 1 << (level - 1);
            for (int i = 0; i <= 8; ++i) {
                const int from = std::min(i / stride * stride, 8 - stride);
                const float t = static_cast<float>(i - from) / static_cast<float>(stride);
                const float line = height(edgeVertex(edge, from)) * (1.0f - t) +
                                   height(edgeVertex(edge, from + stride)) * t;
                deviation = std::max(deviation, std::abs(height(edgeVertex(edge, i)) - line));
            }
        }
    }
    const float depth = deviation + kSkirtMargin;

    mesh.skirtFirstVertex = static_cast<uint32_t>(mesh.vertices.size());
    for (int edge = 0; edge < 4; ++edge) {
        for (int i = 0; i <= 8; ++i) {
            TerrainVertex lowered = mesh.vertices[static_cast<size_t>(edgeVertex(edge, i))];
            lowered.position[2] -= depth;
            mesh.vertices.push_back(lowered);
        }
    }
    const auto skirtVertex = [&](int edge, int i) {
        return static_cast<TerrainIndex>(mesh.skirtFirstVertex + static_cast<uint32_t>(edge * 9 + i));
    };

    std::vector<TerrainIndex> level;
    for (int lod = 1; lod < kTerrainLodLevels; ++lod) {
        const int stride = 1 << (lod - 1);
        const int cells = 8 / stride;
        level.clear();

        // The surface: two triangles a cell, split along the diagonal whose
        // ends are closer in height, which strays least from the full mesh.
        // A cell that is all hole is left open; one that is part hole cannot
        // be drawn at this level, and neither can any coarser one.
        bool offered = true;
        for (int cy = 0; cy < cells && offered; ++cy) {
            for (int cx = 0; cx < cells; ++cx) {
                int holes = 0;
                for (int y = cy * stride; y < (cy + 1) * stride; ++y)
                    for (int x = cx * stride; x < (cx + 1) * stride; ++x) holes += chunk.isHole(y, x) ? 1 : 0;
                if (holes == stride * stride) continue;
                if (holes > 0) { offered = false; break; }

                const auto a = static_cast<TerrainIndex>(outerVertex(cy * stride, cx * stride));
                const auto b = static_cast<TerrainIndex>(outerVertex(cy * stride, (cx + 1) * stride));
                const auto c = static_cast<TerrainIndex>(outerVertex((cy + 1) * stride, cx * stride));
                const auto d = static_cast<TerrainIndex>(outerVertex((cy + 1) * stride, (cx + 1) * stride));
                if (std::abs(height(a) - height(d)) <= std::abs(height(b) - height(c))) {
                    level.insert(level.end(), {a, b, d, a, d, c});
                } else {
                    level.insert(level.end(), {a, b, c, b, d, c});
                }
            }
        }
        if (!offered) break;

        // The skirts, one strip a segment of this level's edge, left off
        // where the ground beside it is a hole.
        for (int edge = 0; edge < 4; ++edge) {
            for (int i = 0; i + stride <= 8; i += stride) {
                bool open = true;
                for (int cell = i; cell < i + stride; ++cell) open = open && edgeCellIsHole(chunk, edge, cell);
                if (open) continue;
                const auto top0 = static_cast<TerrainIndex>(edgeVertex(edge, i));
                const auto top1 = static_cast<TerrainIndex>(edgeVertex(edge, i + stride));
                const TerrainIndex low0 = skirtVertex(edge, i);
                const TerrainIndex low1 = skirtVertex(edge, i + stride);
                level.insert(level.end(), {top0, top1, low1, top0, low1, low0});
            }
        }

        auto& range = mesh.lods[static_cast<size_t>(lod - 1)];
        range.first = static_cast<uint32_t>(mesh.lodIndices.size());
        range.count = static_cast<uint32_t>(level.size());
        mesh.lodIndices.insert(mesh.lodIndices.end(), level.begin(), level.end());
    }
}

void TerrainMeshGenerator::decompressNormal(const int8_t* compressedNormal, float* normal) {
    // WoW stores normals as signed bytes (-127 to 127)
    // Convert to float and normalize

    float x = compressedNormal[0] / 127.0f;
    float y = compressedNormal[1] / 127.0f;
    float z = compressedNormal[2] / 127.0f;

    // Normalize
    float length = std::sqrt(x * x + y * y + z * z);
    if (length > 0.0001f) {
        normal[0] = x / length;
        normal[1] = y / length;
        normal[2] = z / length;
    } else {
        // Default up normal if degenerate
        normal[0] = 0.0f;
        normal[1] = 0.0f;
        normal[2] = 1.0f;
    }
}

bool TerrainMeshGenerator::chunkFractionsAt(const float chunkPosition[3],
                                            float glX, float glY, float unitSize,
                                            float& fracX, float& fracY) {
    const float maxX = chunkPosition[0];
    const float minX = chunkPosition[0] - 8.0f * unitSize;
    const float maxY = chunkPosition[1];
    const float minY = chunkPosition[1] - 8.0f * unitSize;
    if (glX < minX || glX > maxX || glY < minY || glY > maxY) return false;

    fracY = std::clamp((chunkPosition[0] - glX) / unitSize, 0.0f, 8.0f);
    fracX = std::clamp((chunkPosition[1] - glY) / unitSize, 0.0f, 8.0f);
    return true;
}

glm::vec3 TerrainMeshGenerator::chunkSurfacePoint(const float chunkPosition[3],
                                                  const HeightMap& heightMap,
                                                  float fracX, float fracY,
                                                  float unitSize) {
    // The axes cross: world X runs against grid Y and world Y against grid X.
    const float worldX = chunkPosition[0] - fracY * unitSize;
    const float worldY = chunkPosition[1] - fracX * unitSize;

    // The surface is four triangles fanned from the quad's centre vertex, which
    // is what generateChunkMesh emits and therefore what the player sees.
    //
    // Interpolating the four outer corners bilinearly instead ignores that
    // centre vertex completely, and MCVT puts it wherever the terrain artist
    // needed it - on a hillside it commonly sits a yard or two off the plane of
    // its corners. The floor query then answered lower than the ground being
    // drawn and the player sank through a slope that looked gentle.
    const int qx = glm::clamp(static_cast<int>(std::floor(fracX)), 0, 7);
    const int qy = glm::clamp(static_cast<int>(std::floor(fracY)), 0, 7);
    const float u = glm::clamp(fracX - static_cast<float>(qx), 0.0f, 1.0f);
    const float v = glm::clamp(fracY - static_cast<float>(qy), 0.0f, 1.0f);

    const float hTL = heightMap.getHeight(qx,     qy);
    const float hTR = heightMap.getHeight(qx + 1, qy);
    const float hBL = heightMap.getHeight(qx,     qy + 1);
    const float hBR = heightMap.getHeight(qx + 1, qy + 1);
    // Centre vertex of this quad in the interleaved 9x17 grid, at (0.5, 0.5).
    const int centreIndex = 9 + qy * 17 + qx;
    const float hC = (centreIndex >= 0 && centreIndex < static_cast<int>(heightMap.heights.size()))
                     ? heightMap.heights[centreIndex]
                     : 0.25f * (hTL + hTR + hBL + hBR);

    // Which of the four wedges the point is in, then barycentric within it.
    // Each wedge is the centre plus one edge of the quad, so two of the three
    // weights are the corner heights and the third is always the centre.
    float hA, hB;      // the two corners of the wedge
    float wA, wB;      // their weights
    if (u > v) {
        if (u + v < 1.0f) {          // top wedge: TL, TR
            hA = hTL; hB = hTR; wA = 1.0f - u - v; wB = u - v;
        } else {                      // right wedge: TR, BR
            hA = hTR; hB = hBR; wA = u - v;        wB = u + v - 1.0f;
        }
    } else {
        if (u + v < 1.0f) {          // left wedge: TL, BL
            hA = hTL; hB = hBL; wA = 1.0f - u - v; wB = v - u;
        } else {                      // bottom wedge: BL, BR
            hA = hBL; hB = hBR; wA = v - u;        wB = u + v - 1.0f;
        }
    }
    const float wC = 1.0f - wA - wB;
    const float worldZ = chunkPosition[2] + (hA * wA + hB * wB + hC * wC);
    return glm::vec3(worldX, worldY, worldZ);
}
} // namespace pipeline
} // namespace wowee
