#include "pipeline/wmo_bsp.hpp"
#include <cmath>
#include <unordered_set>
#include <utility>

namespace wowee::pipeline {
std::shared_ptr<const WmoBspData> WmoBspData::build(
    std::vector<WmoBspNode> nodes, std::vector<uint16_t> faces,
    std::span<const uint16_t> indices, size_t vertexCount, std::string& error) {
    error.clear();
    if (nodes.empty()) { error = "MOBN has no root node"; return {}; }
    if (indices.size()%3) { error = "MOVI is not a triangle list"; return {}; }
    for (auto index : indices) {
        if (index >= vertexCount) { error = "MOVI references a missing vertex"; return {}; }
    }
    for (auto face : faces) {
        if (face >= indices.size()/3) { error = "MOBR references a missing triangle"; return {}; }
    }
    for (size_t i=0; i<nodes.size(); ++i) {
        const auto& n = nodes[i];
        if (!std::isfinite(n.planeDistance)) {
            error = "MOBN node " + std::to_string(i) + " has a nonfinite plane";
            return {};
        }
        if (n.flags & 4) {
            if (uint64_t(n.firstFace) + n.faceCount > faces.size()) {
                error = "MOBN leaf " + std::to_string(i) + " exceeds MOBR";
                return {};
            }
        } else {
            if ((n.flags & 3) == 3) {
                error = "MOBN node " + std::to_string(i) + " has an invalid split axis";
                return {};
            }
            for (auto child : {n.negativeChild, n.positiveChild}) {
                if (child != 0xffff && child >= nodes.size()) {
                    error = "MOBN node " + std::to_string(i) + " references a missing child";
                    return {};
                }
            }
        }
    }
    // Iterative DFS avoids trusting the file's depth on the worker stack.
    std::vector<uint8_t> color(nodes.size(), 0);
    std::vector<std::pair<size_t, bool>> stack;
    for (size_t root=0; root<nodes.size(); ++root) {
        if (color[root]) continue;
        stack.emplace_back(root, false);
        while (!stack.empty()) {
            auto [index, exiting] = stack.back();
            stack.pop_back();
            if (exiting) { color[index] = 2; continue; }
            if (color[index] == 2) continue;
            if (color[index] == 1) { error = "MOBN contains a child cycle"; return {}; }
            color[index] = 1;
            stack.emplace_back(index, true);
            if (!(nodes[index].flags & 4)) {
                for (auto child : {nodes[index].negativeChild, nodes[index].positiveChild}) {
                    if (child != 0xffff) stack.emplace_back(child, false);
                }
            }
        }
    }
    auto result = std::make_shared<WmoBspData>();
    result->nodes = std::move(nodes);
    result->faces = std::move(faces);
    result->digestEligible.resize(result->nodes.size(), false);
    std::unordered_set<uint16_t> vertices;
    vertices.reserve(451);
    for (size_t i=0; i<result->nodes.size(); ++i) {
        const auto& n = result->nodes[i];
        if (!(n.flags & 4) || n.faceCount > 300) continue;
        vertices.clear();
        for (size_t f=n.firstFace; f<size_t(n.firstFace)+n.faceCount; ++f) {
            const size_t first = size_t(result->faces[f])*3;
            vertices.insert(indices[first]);
            vertices.insert(indices[first+1]);
            vertices.insert(indices[first+2]);
            if (vertices.size() > 450) break;
        }
        result->digestEligible[i] = vertices.size() <= 450;
    }
    return result;
}
} // namespace wowee::pipeline
