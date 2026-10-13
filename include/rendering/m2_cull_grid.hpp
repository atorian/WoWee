#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace rendering {

/// M2 instances filed by position, so a pass can throw away whole regions of
/// the world before it looks at a single instance in them.
///
/// The view pass and the shadow pass used to visit every instance resident -
/// 92 thousand in Dalaran and 123 thousand in Mulgore at the longest view
/// distance - each a cache miss or three in a 616-byte struct, to keep the few
/// thousand that could be drawn. Here each instance lives in exactly one 64-yard
/// column, by its position, and each column keeps bounds that contain
/// everything the per-instance tests look at:
///
/// - viewMin/viewMax: every instance's position and its cull sphere (centre and
///   padded radius), so a column outside the frustum or beyond the furthest
///   any instance may be drawn holds nothing the view pass would have kept;
/// - posMin/posMax and maxShadowRadius: every instance's position, and the
///   largest margin the shadow cull inflates a position by, so a column whose
///   positions are outside the light's box by more than that margin holds no
///   caster.
///
/// Rejecting a column therefore never drops an instance the per-instance test
/// would have accepted. Bounds only grow while a column is in use (a removal
/// leaves them as they were), which keeps them conservative; an emptied column
/// starts over.
class M2CullGrid {
public:
    static constexpr float kCellSize = 64.0f;

    struct Cell {
        glm::vec3 viewMin{std::numeric_limits<float>::max()};
        glm::vec3 viewMax{std::numeric_limits<float>::lowest()};
        glm::vec3 posMin{std::numeric_limits<float>::max()};
        glm::vec3 posMax{std::numeric_limits<float>::lowest()};
        float maxShadowRadius = 0.0f;
        std::vector<uint32_t> ids;
    };

    struct Entry {
        glm::vec3 position;
        glm::vec3 cullCenter;
        float cullRadius;
        float shadowRadius;
    };

    void clear() {
        cells_.clear();
        cellIndex_.clear();
        cellOfId_.clear();
    }

    void insert(uint32_t id, const Entry& e) {
        const uint32_t c = cellFor(e.position);
        Cell& cell = cells_[c];
        cell.ids.push_back(id);
        grow(cell, e);
        cellOfId_[id] = c;
    }

    void remove(uint32_t id) {
        const auto it = cellOfId_.find(id);
        if (it == cellOfId_.end()) return;
        Cell& cell = cells_[it->second];
        for (size_t k = 0; k < cell.ids.size(); ++k) {
            if (cell.ids[k] != id) continue;
            cell.ids[k] = cell.ids.back();
            cell.ids.pop_back();
            break;
        }
        if (cell.ids.empty()) reset(cell);
        cellOfId_.erase(it);
    }

    /// After an instance moved or its cull data changed.
    void update(uint32_t id, const Entry& e) {
        const auto it = cellOfId_.find(id);
        if (it != cellOfId_.end() && it->second == cellFor(e.position)) {
            grow(cells_[it->second], e);
            return;
        }
        remove(id);
        insert(id, e);
    }

    [[nodiscard]] const std::vector<Cell>& cells() const { return cells_; }
    [[nodiscard]] size_t size() const { return cellOfId_.size(); }

private:
    static uint64_t keyFor(const glm::vec3& p) {
        const auto x = static_cast<int32_t>(std::floor(p.x / kCellSize));
        const auto y = static_cast<int32_t>(std::floor(p.y / kCellSize));
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
    }

    uint32_t cellFor(const glm::vec3& p) {
        const uint64_t key = keyFor(p);
        const auto it = cellIndex_.find(key);
        if (it != cellIndex_.end()) return it->second;
        const auto index = static_cast<uint32_t>(cells_.size());
        cells_.emplace_back();
        cellIndex_.emplace(key, index);
        return index;
    }

    static void grow(Cell& cell, const Entry& e) {
        const glm::vec3 r(e.cullRadius);
        cell.viewMin = glm::min(cell.viewMin, glm::min(e.position, e.cullCenter - r));
        cell.viewMax = glm::max(cell.viewMax, glm::max(e.position, e.cullCenter + r));
        cell.posMin = glm::min(cell.posMin, e.position);
        cell.posMax = glm::max(cell.posMax, e.position);
        cell.maxShadowRadius = std::max(cell.maxShadowRadius, e.shadowRadius);
    }

    static void reset(Cell& cell) {
        std::vector<uint32_t> ids;
        ids.swap(cell.ids);
        cell = Cell{};
        cell.ids.swap(ids);
    }

    std::vector<Cell> cells_;
    std::unordered_map<uint64_t, uint32_t> cellIndex_;
    std::unordered_map<uint32_t, uint32_t> cellOfId_;
};

/// Whether any position inside [posMin, posMax] could pass the shadow pass's
/// per-instance test against this light-space (orthographic) matrix with a
/// margin of up to maxMarginNdc: |x|,|y| <= 1 + m and -m <= z <= 1 + m.
inline bool cellMayCastShadow(const glm::mat4& lightSpace, const glm::vec3& posMin,
                              const glm::vec3& posMax, float maxMarginNdc) {
    if (posMin.x > posMax.x) return false;  // empty
    const glm::vec3 c = (posMin + posMax) * 0.5f;
    const glm::vec3 h = (posMax - posMin) * 0.5f;
    for (int row = 0; row < 3; ++row) {
        const glm::vec3 axis(lightSpace[0][row], lightSpace[1][row], lightSpace[2][row]);
        const float centre = glm::dot(axis, c) + lightSpace[3][row];
        const float extent = glm::dot(glm::abs(axis), h);
        const float lo = row < 2 ? -1.0f - maxMarginNdc : -maxMarginNdc;
        const float hi = 1.0f + maxMarginNdc;
        if (centre - extent > hi || centre + extent < lo) return false;
    }
    return true;
}

/// Squared distance from p to the nearest point of [mn, mx].
inline float distanceSqToBox(const glm::vec3& p, const glm::vec3& mn, const glm::vec3& mx) {
    const glm::vec3 q = glm::clamp(p, mn, mx);
    const glm::vec3 d = p - q;
    return glm::dot(d, d);
}

} // namespace rendering
} // namespace wowee
