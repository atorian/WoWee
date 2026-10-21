#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <glm/glm.hpp>
namespace wowee::pipeline {
// WDL coordinates match ADT tile x/y. One sample cell covers one ADT chunk.
struct DistantTile {
    int x = 0, y = 0;
    std::array<int16_t, 289> outer{};
    std::array<int16_t, 256> inner{};
    std::array<uint16_t, 16> holes{};
};
struct DistantMap { std::vector<DistantTile> tiles; bool valid = false; };
DistantMap readDistantMap(std::span<const uint8_t> bytes);
struct DistantVertex { glm::vec3 position, normal, color; glm::vec2 uv; };
struct DistantMesh {
    std::vector<DistantVertex> vertices;
    std::vector<uint32_t> indices;
    struct Range { uint32_t first = 0, count = 0; float error = 0; };
    std::array<Range, 4> lods{};
    glm::vec3 min{}, max{};
};
DistantMesh distantTileMesh(const DistantTile& tile);
using TerrainCoverage = std::array<std::array<uint32_t, 8>, 4096>;
}
