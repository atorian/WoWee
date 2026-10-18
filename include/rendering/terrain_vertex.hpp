#pragma once

#include <array>
#include <cstddef>

#include "pipeline/terrain_mesh.hpp"
#include "rendering/vertex_layout.hpp"

namespace wowee {
namespace rendering {

/// How the terrain vertex is read, stated once.
///
/// terrain_renderer.cpp described this three times: twice for the two pipeline
/// builders, and once more for the shadow pass with the offsets as literals
/// beside a comment asserting a stride of 44. That stride is not a number
/// anyone chose; it is what the trailing `chunkIndex` byte pads out to, so a
/// field added or reordered in TerrainVertex moves it without touching the
/// comment.

/// What assets/shaders/terrain.vert.glsl declares, in its order.
inline constexpr std::array<VertexAttribute, 4> kTerrainVertexAttributes = {{
    {.location = 0, .componentCount = 3, .offset = static_cast<uint32_t>(offsetof(pipeline::TerrainVertex, position))},
    {.location = 1, .componentCount = 3, .offset = static_cast<uint32_t>(offsetof(pipeline::TerrainVertex, normal))},
    {.location = 2, .componentCount = 2, .offset = static_cast<uint32_t>(offsetof(pipeline::TerrainVertex, texCoord))},
    {.location = 3, .componentCount = 2, .offset = static_cast<uint32_t>(offsetof(pipeline::TerrainVertex, layerUV))},
}};

/// The same geometry through the shadow shader (shadow.vert.glsl): the
/// position, and the texture coordinate it passes on.
inline constexpr std::array<VertexAttribute, 2> kTerrainShadowVertexAttributes = {{
    {.location = 0, .componentCount = 3, .offset = static_cast<uint32_t>(offsetof(pipeline::TerrainVertex, position))},
    {.location = 1, .componentCount = 2, .offset = static_cast<uint32_t>(offsetof(pipeline::TerrainVertex, texCoord))},
}};

}  // namespace rendering
}  // namespace wowee
