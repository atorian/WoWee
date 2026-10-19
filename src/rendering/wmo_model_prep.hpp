#pragma once

// What WMORenderer::prepareModel works out for a model before its upload.
// Private to the WMO renderer: others hold it by pointer and hand it back.

#include "rendering/rt_scene.hpp"
#include "rendering/wmo_batch_merge.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/wmo_vertex.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace rendering {

struct WmoModelPrep {
    /// Batches that draw as one, with everything but what loading the
    /// textures decides: the texture itself, its normal map, its material.
    struct Merge {
        WmoBatchState state;
        std::vector<WMORenderer::GroupResources::MergedBatch::DrawRange> draws;
    };

    /// A group that draws, in the model's order.
    struct Group {
        /// Everything but the buffers and the merged batches.
        WMORenderer::GroupResources resources;
        /// What the vertex and index buffers are to hold; let go once they do.
        std::vector<WMOVertex> vertices;
        std::vector<uint16_t> indices;
        std::vector<Merge> merges;
    };

    glm::vec3 boundingBoxMin{0.0f};
    glm::vec3 boundingBoxMax{0.0f};
    glm::vec3 ambientColor{0.5f};
    bool isLowPlatform = false;

    /// The texture list as the model names it, for loading, and lowercased,
    /// which is what material detection and retries read.
    std::vector<std::string> texturePaths;
    std::vector<std::string> textureNames;
    std::vector<uint32_t> materialTextureIndices;
    std::vector<uint32_t> materialBlendModes;
    std::vector<uint32_t> materialFlags;
    std::vector<uint32_t> materialSidnColors;

    std::vector<Group> groups;

    std::vector<WMORenderer::PortalData> portals;
    std::vector<glm::vec3> portalVertices;
    std::vector<WMORenderer::PortalRef> portalRefs;
    std::vector<std::pair<uint16_t, uint16_t>> groupPortalRefs;
    std::vector<WMORenderer::DoodadTemplate> doodadTemplates;

    /// The model's mesh for the ray-traced scene, all but the colour of its
    /// triangles: that is read from the loaded textures, by each triangle's
    /// material.
    RtScene::MeshSource rtMesh;
    std::vector<uint32_t> rtTriangleMaterial;
};

} // namespace rendering
} // namespace wowee
