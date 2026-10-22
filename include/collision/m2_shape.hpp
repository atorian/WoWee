#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::collision {

/// What a doodad (an M2) is solid with: its authored collision mesh, in model
/// space, binned into a grid of triangles.
///
/// Built once per model and shared by every placement of it - and by the
/// renderer, which drew the model and used to keep this inside its GPU model.
/// Immutable once built, so the queries may read it from any thread.
///
/// A model with no collision mesh has no shape. So does one that is not there
/// to collide with though it has a mesh: an invisible trap volume, a spell
/// effect. Only a collision mesh blocks, as in the client - a model without
/// one used to be stopped by a box made from its bounds and shaped by its name.
struct M2Shape {
    /// The model's path, for the logs: a doodad that blocks unexpectedly is
    /// reported as "stuck on this", and the name is what finds its mesh.
    std::string name;
    std::vector<glm::vec3> vertices;
    std::vector<uint16_t> indices;
    uint32_t triCount = 0;
    /// The box around the vertices.
    glm::vec3 boundMin{0.0f};
    glm::vec3 boundMax{0.0f};

    struct TriBounds { float minZ, maxZ; };
    std::vector<TriBounds> triBounds;

    static constexpr float kCellSize = 4.0f;
    glm::vec2 gridOrigin{0.0f};
    int gridCellsX = 0;
    int gridCellsY = 0;
    /// All valid triangles. A placement can rotate any model-space facet
    /// into a world-space floor; its role cannot be fixed when loading it.
    std::vector<std::vector<uint32_t>> cellTris;
    /// Model-space walls used by the legacy wall response. The original
    /// contact solver consumes unclassified facets instead.
    std::vector<std::vector<uint32_t>> cellWallTris;

    /// The shape of a mesh, or nothing for one with no triangle in it.
    [[nodiscard]] static std::shared_ptr<const M2Shape> build(std::string name,
                                                             std::vector<glm::vec3> vertices,
                                                             std::vector<uint16_t> indices);

    /// The triangles a model-space XY box reaches, each once in mesh order.
    void trisInRange(float minX, float minY, float maxX, float maxY,
                          std::vector<uint32_t>& out) const;
    void wallTrisInRange(float minX, float minY, float maxX, float maxY,
                         std::vector<uint32_t>& out) const;

    /// The memory it holds, for the resource monitor.
    [[nodiscard]] uint64_t bytes() const;

private:
    void gatherTrisInRange(const std::vector<std::vector<uint32_t>>& cells,
                           float minX, float minY, float maxX, float maxY,
                           std::vector<uint32_t>& out) const;
};

}  // namespace wowee::collision
