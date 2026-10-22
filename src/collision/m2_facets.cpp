#include "collision/m2_facets.hpp"
#include <cmath>

namespace wowee::collision {
std::vector<MovementFacet> queryM2Facets(std::span<const glm::vec3> vertices,
    std::span<const uint16_t> indices, std::span<const glm::vec3> normals,
    const glm::mat4& transform, const glm::vec3& queryMin,
    const glm::vec3& queryMax, uint64_t owner) {
    std::vector<glm::vec3> positions(vertices.size());
    std::vector<uint8_t> codes(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto p = vertices[i];
        for (int axis = 0; axis < 3; ++axis) {
            positions[i][axis] = float((double(transform[2][axis])*p.z + double(transform[1][axis])*p.y +
                                       double(transform[0][axis])*p.x) + transform[3][axis]);
            if (positions[i][axis] < queryMin[axis]) codes[i] |= uint8_t(1 << (axis*2));
            else if (positions[i][axis] > queryMax[axis]) codes[i] |= uint8_t(2 << (axis*2));
        }
    }
    // The original removes each basis vector's scale independently. Normals
    // come from the file, not a cross product or an inverse-transpose matrix.
    glm::mat3 rotation(transform);
    for (int column = 0; column < 3; ++column) {
        const glm::dvec3 p(rotation[column]);
        const double length = column == 0 ? p.y*p.y + p.z*p.z + p.x*p.x
                                         : p.z*p.z + p.y*p.y + p.x*p.x;
        if (length > 0.00000023841858f)
            rotation[column] = p * (1.0 / std::sqrt(length));
    }
    std::vector<MovementFacet> result;
    result.reserve(normals.size());
    for (size_t face = 0; face < normals.size(); ++face) {
        const auto a = indices[face*3], b = indices[face*3+1], c = indices[face*3+2];
        if (codes[a] & codes[b] & codes[c]) continue;
        MovementFacet facet;
        facet.owner = owner;
        facet.vertices = {positions[a], positions[b], positions[c]};
        const glm::dvec3 n(normals[face]);
        auto& out = facet.plane.normal;
        out.x = float(n.y*rotation[1].x + n.z*rotation[2].x + n.x*rotation[0].x);
        out.y = float(n.z*rotation[2].y + n.x*rotation[0].y + n.y*rotation[1].y);
        out.z = float(n.x*rotation[0].z + n.y*rotation[1].z + n.z*rotation[2].z);
        const auto p = facet.vertices[0];
        facet.plane.offset = float(-(double(p.z)*out.z + double(p.y)*out.y + double(p.x)*out.x));
        result.push_back(facet);
    }
    return result;
}
}
