#include "pipeline/detail_doodads.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "core/coordinates.hpp"
#include "pipeline/client_random.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/terrain_mesh.hpp"

namespace wowee {
namespace pipeline {

namespace {

constexpr uint32_t kDoodadTableSize = 16;
// Placements whose triangle's normal points less than this far up are
// dropped (0x7d3ae3).
constexpr float kMinGroundNormalZ = 0.4f;
constexpr float kScaleSpread = 0.33f;
constexpr uint32_t kDefaultDoodadsPerQuad = 8;

// The 16 doodad slots a quad picks from in turn, filled by weight. Each
// weight unit takes the next slot 13 along, wrapping, so equal weights
// interleave; weights adding past 16 overwrite earlier slots, and weights
// adding to less fill the rest from the four doodads in order (0x7d395d).
std::array<uint32_t, kDoodadTableSize> doodadSlots(const GroundEffectTexture& effect) {
    std::array<uint32_t, kDoodadTableSize> slots{};
    uint32_t cursor = 0;
    uint32_t filled = 0;
    for (size_t d = 0; d < 4; ++d) {
        for (uint32_t w = 0; w < effect.weights[d]; ++w) {
            slots[cursor % kDoodadTableSize] = effect.doodadIds[d];
            cursor += 13;
        }
        filled += effect.weights[d];
    }
    for (; filled < kDoodadTableSize; ++filled) {
        slots[cursor % kDoodadTableSize] = effect.doodadIds[filled % 4];
        cursor += 13;
    }
    return slots;
}

void noteOnce(std::vector<uint32_t>& ids, uint32_t id) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
}

} // namespace

DetailDoodadModel detailDoodadModelFrom(const M2Model& model, uint32_t* skippedBatches) {
    DetailDoodadModel out;
    out.vertices.reserve(model.vertices.size());
    for (const M2Vertex& v : model.vertices) out.vertices.push_back({v.position, v.texCoords[0]});
    out.indices = model.indices;
    uint32_t skipped = 0;
    for (const M2Batch& batch : model.batches) {
        if (batch.indexCount == 0) continue;
        const std::string* texture = nullptr;
        if (batch.textureIndex < model.textureLookup.size()) {
            const uint16_t t = model.textureLookup[batch.textureIndex];
            if (t < model.textures.size() && model.textures[t].type == 0 &&
                !model.textures[t].filename.empty()) {
                texture = &model.textures[t].filename;
            }
        }
        if (!texture || batch.indexStart + batch.indexCount > out.indices.size()) {
            ++skipped;
            continue;
        }
        out.batches.push_back({*texture, batch.indexStart, batch.indexCount});
    }
    if (skippedBatches) *skippedBatches = skipped;
    return out;
}

glm::mat3 detailDoodadBasis(const DetailDoodadPlacement& placement, bool liesOnGround) {
    const float c = std::cos(placement.rotation);
    const float s = std::sin(placement.rotation);
    // Columns: where the model's x, y and z go.
    glm::mat3 turn(glm::vec3(c, s, 0.0f), glm::vec3(-s, c, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat3 basis = turn * placement.scale;
    if (!liesOnGround) return basis;

    // The shortest rotation taking up onto the ground's normal.
    const glm::vec3 n = glm::normalize(placement.groundNormal);
    const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), n);
    const float cosAngle = n.z;
    if (cosAngle > 0.99999f) return basis;
    const glm::mat3 k(glm::vec3(0.0f, axis.z, -axis.y), glm::vec3(-axis.z, 0.0f, axis.x),
                      glm::vec3(axis.y, -axis.x, 0.0f));
    const glm::mat3 tilt = glm::mat3(1.0f) + k + k * k * (1.0f / (1.0f + cosAngle));
    return tilt * basis;
}

DetailChunkMesh buildDetailChunkMesh(
    const std::vector<DetailDoodadPlacement>& placements, const GroundEffectTables& tables,
    const std::function<const DetailDoodadModel*(uint32_t doodadId)>& modelFor) {
    // Gathered per texture first, so each texture is one batch however many
    // doodads in the chunk use it.
    struct Group {
        std::string texture;
        std::vector<uint32_t> indices;
    };
    std::vector<Group> groups;
    auto groupFor = [&](const std::string& texture) -> Group& {
        for (Group& g : groups) {
            if (g.texture == texture) return g;
        }
        groups.push_back({texture, {}});
        return groups.back();
    };

    DetailChunkMesh mesh;
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (const DetailDoodadPlacement& p : placements) {
        const DetailDoodadModel* model = modelFor(p.doodadId);
        if (!model || model->batches.empty()) continue;
        const auto doodad = tables.doodads.find(p.doodadId);
        const bool lies = doodad != tables.doodads.end() && doodad->second.liesOnGround();
        const glm::mat3 basis = detailDoodadBasis(p, lies);

        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        for (const DetailDoodadModel::Vertex& v : model->vertices) {
            DetailChunkMesh::Vertex out;
            out.position = p.position + basis * v.position;
            out.normal = p.groundNormal;
            out.uv = v.uv;
            lo = glm::min(lo, out.position);
            hi = glm::max(hi, out.position);
            mesh.vertices.push_back(out);
        }
        for (const DetailDoodadModel::Batch& b : model->batches) {
            Group& g = groupFor(b.texture);
            for (uint32_t i = 0; i < b.indexCount; ++i) {
                g.indices.push_back(base + model->indices[b.firstIndex + i]);
            }
        }
    }
    for (Group& g : groups) {
        DetailChunkMesh::Batch batch;
        batch.texture = std::move(g.texture);
        batch.firstIndex = static_cast<uint32_t>(mesh.indices.size());
        batch.indexCount = static_cast<uint32_t>(g.indices.size());
        mesh.indices.insert(mesh.indices.end(), g.indices.begin(), g.indices.end());
        mesh.batches.push_back(std::move(batch));
    }
    if (!mesh.vertices.empty()) {
        mesh.boundsMin = lo;
        mesh.boundsMax = hi;
    }
    return mesh;
}

uint32_t detailDoodadSeed(uint32_t globalChunkX, uint32_t globalChunkY) {
    return (globalChunkY << 16) | globalChunkX;
}

DetailDoodadLayout placeDetailDoodads(const MapChunk& chunk,
                                      uint32_t globalChunkX, uint32_t globalChunkY,
                                      const GroundEffectTables& tables,
                                      uint32_t quadDraws) {
    DetailDoodadLayout layout;
    if (!chunk.hasHeightMap()) return layout;

    ClientRandom random(detailDoodadSeed(globalChunkX, globalChunkY));

    // First every quad is drawn, then each draw is planted, in that order: the
    // second pass's draws come after all of the first's in the sequence.
    struct Quad { uint32_t x, y; };
    std::vector<Quad> quads(quadDraws);
    for (Quad& q : quads) {
        q.x = random.next() & 7u;
        q.y = random.next() & 7u;
    }

    // A quad: a sixteenth of a tile's chunk, an eighth of that.
    constexpr float unit = core::coords::TILE_SIZE / 128.0f;
    for (uint32_t draw = 0; draw < quadDraws; ++draw) {
        const Quad q = quads[draw];
        const int qx = static_cast<int>(q.x);
        const int qy = static_cast<int>(q.y);
        if (chunk.isEffectDisabled(qy, qx)) continue;
        if (chunk.isHole(qy, qx)) continue;

        const uint32_t layerIndex = chunk.effectLayerFor(qy, qx);
        if (layerIndex >= chunk.layers.size()) {
            ++layout.missingLayers;
            continue;
        }
        const uint32_t effectId = chunk.layers[layerIndex].effectId;
        if (effectId == 0) continue;  // the layer grows nothing
        const auto effectIt = tables.textures.find(effectId);
        if (effectIt == tables.textures.end()) {
            noteOnce(layout.unknownEffects, effectId);
            continue;
        }
        const GroundEffectTexture& effect = effectIt->second;
        const auto slots = doodadSlots(effect);
        const uint32_t count = effect.density != 0 ? effect.density : kDefaultDoodadsPerQuad;

        for (uint32_t k = 0; k < count; ++k) {
            // Column offset first, then row, each over the whole quad.
            const float u = random.nextSigned();
            const float v = random.nextSigned();
            const uint32_t doodadId = slots[(k + draw) % kDoodadTableSize];
            if (doodadId == 0) continue;
            if (tables.doodads.find(doodadId) == tables.doodads.end()) {
                noteOnce(layout.unknownDoodads, doodadId);
                continue;
            }

            const float fracX = static_cast<float>(qx) + (u + 1.0f) * 0.5f;
            const float fracY = static_cast<float>(qy) + (v + 1.0f) * 0.5f;
            const ChunkSurfaceSample ground = TerrainMeshGenerator::chunkSurfaceSample(
                chunk.position, chunk.heightMap, fracX, fracY, unit);
            if (ground.normal.z < kMinGroundNormalZ) continue;

            DetailDoodadPlacement p;
            p.doodadId = doodadId;
            p.position = ground.point;
            p.groundNormal = ground.normal;
            p.rotation = (random.nextSigned() + 1.0f) * core::coords::PI;
            p.scale = random.nextSigned() * kScaleSpread + 1.0f;
            layout.placements.push_back(p);
        }
    }
    return layout;
}

} // namespace pipeline
} // namespace wowee
