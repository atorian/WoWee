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
///
/// A column lists its instances by their index in the instance list, which a
/// pass reads directly; the instance id once had to be looked up in a hash
/// map for each of the tens of thousands of candidates. The list is kept in
/// step with the instance list's swap-remove (removeSwapLast), as the
/// renderer's other index sets are. Each index also has its Record: what the
/// per-instance tests read, in a dense array of its own.
class M2CullGrid {
public:
    static constexpr float kCellSize = 64.0f;

    /// What the passes' per-instance tests read of instance `index`, apart
    /// from the rest of it: 48 bytes, where the instance is 600. A cull over
    /// the candidates of a city reads these, one or two to a cache line, and
    /// the instance only for those it keeps. M2Renderer::fileInCullGrid
    /// writes the instance's part from the instance whenever it changes; the
    /// GPU cull's verdict is the cull's own and lives only here.
    struct Record {
        glm::vec3 position{0.0f};
        /// The cull sphere: the visual bounds' centre and padded radius.
        float paddedRadius = 0.0f;
        glm::vec3 cullCenter{0.0f};
        /// Times the frame's render distance squared: its own range.
        float maxDistSqFactor = 1.0f;
        uint32_t id = 0;
        /// How far beyond its position the shadow pass lets it cast: its
        /// bound radius at its scale, times 1.5.
        float shadowRadius = 0.0f;
        /// The render() call whose GPU verdict lastVisible is; a verdict from
        /// another call is not this frame's.
        uint32_t verdictSerial = 0;
        uint16_t flags = 0;
        /// The last GPU verdict that covered it. Visible until one says not,
        /// so a new instance is drawn at once.
        uint8_t lastVisible = 1;
        /// Frames in a row the GPU culled it, capped at 3, for the HiZ test's
        /// hysteresis. A new one counts as not recently seen.
        uint8_t culledFrames = 2;
    };
    enum Flag : uint16_t {
        kValid = 1 << 0,          ///< its model is valid
        kHasModel = 1 << 1,
        kInvisibleTrap = 1 << 2,
        kGroundDetail = 1 << 3,
        kAnimatedSkyBird = 1 << 4, ///< a sky bird that animates: a range of its own
        kGameObject = 1 << 5,
        kHidden = 1 << 6,          ///< out of the world, and in no column
        kFinishing = 1 << 7,       ///< ended: its mesh is not drawn
    };

    struct Cell {
        glm::vec3 viewMin{std::numeric_limits<float>::max()};
        glm::vec3 viewMax{std::numeric_limits<float>::lowest()};
        glm::vec3 posMin{std::numeric_limits<float>::max()};
        glm::vec3 posMax{std::numeric_limits<float>::lowest()};
        float maxShadowRadius = 0.0f;
        /// The instances filed here, by their index in the instance list.
        std::vector<uint32_t> members;
    };

    void clear() {
        cells_.clear();
        cellIndex_.clear();
        records_.clear();
        cellOf_.clear();
        slotOf_.clear();
    }

    /// A record for the instance just added at the end of the list.
    void add() {
        records_.emplace_back();
        cellOf_.push_back(kNone);
        slotOf_.push_back(0);
    }

    [[nodiscard]] size_t size() const { return records_.size(); }
    [[nodiscard]] Record& record(uint32_t index) { return records_[index]; }
    [[nodiscard]] const Record& record(uint32_t index) const { return records_[index]; }

    /// Files instance `index` in the column its record's position is in, or
    /// keeps it where it is and grows the column's bounds; a hidden one goes
    /// out of any column.
    void file(uint32_t index) {
        const Record& r = records_[index];
        if (r.flags & kHidden) {
            unfile(index);
            return;
        }
        const uint32_t c = cellFor(r.position);
        if (cellOf_[index] != c) {
            unfile(index);
            Cell& cell = cells_[c];
            cellOf_[index] = c;
            slotOf_[index] = static_cast<uint32_t>(cell.members.size());
            cell.members.push_back(index);
        }
        grow(cells_[c], r);
    }

    /// The instance at `index` is gone and the last one takes its place, as
    /// the instance list does it (swap-remove).
    void removeSwapLast(uint32_t index) {
        const auto last = static_cast<uint32_t>(records_.size() - 1);
        unfile(index);
        if (index != last) {
            records_[index] = records_[last];
            const uint32_t c = cellOf_[last];
            cellOf_[index] = c;
            slotOf_[index] = slotOf_[last];
            if (c != kNone) cells_[c].members[slotOf_[last]] = index;
        }
        records_.pop_back();
        cellOf_.pop_back();
        slotOf_.pop_back();
    }

    [[nodiscard]] const std::vector<Cell>& cells() const { return cells_; }
    /// How many instances are in a column.
    [[nodiscard]] size_t filed() const {
        size_t n = 0;
        for (const uint32_t c : cellOf_) n += c != kNone;
        return n;
    }

private:
    static constexpr uint32_t kNone = ~0u;

    void unfile(uint32_t index) {
        const uint32_t c = cellOf_[index];
        if (c == kNone) return;
        Cell& cell = cells_[c];
        const uint32_t slot = slotOf_[index];
        const uint32_t moved = cell.members.back();
        cell.members[slot] = moved;
        slotOf_[moved] = slot;
        cell.members.pop_back();
        if (cell.members.empty()) reset(cell);
        cellOf_[index] = kNone;
    }

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

    static void grow(Cell& cell, const Record& r) {
        const glm::vec3 radius(r.paddedRadius);
        cell.viewMin = glm::min(cell.viewMin, glm::min(r.position, r.cullCenter - radius));
        cell.viewMax = glm::max(cell.viewMax, glm::max(r.position, r.cullCenter + radius));
        cell.posMin = glm::min(cell.posMin, r.position);
        cell.posMax = glm::max(cell.posMax, r.position);
        cell.maxShadowRadius = std::max(cell.maxShadowRadius, r.shadowRadius);
    }

    static void reset(Cell& cell) {
        std::vector<uint32_t> members;
        members.swap(cell.members);
        cell = Cell{};
        cell.members.swap(members);
    }

    std::vector<Cell> cells_;
    std::unordered_map<uint64_t, uint32_t> cellIndex_;
    /// By instance index: its record, its column (kNone for none) and its
    /// place in that column's members.
    std::vector<Record> records_;
    std::vector<uint32_t> cellOf_;
    std::vector<uint32_t> slotOf_;
};
static_assert(sizeof(M2CullGrid::Record) == 48, "two to three records to a 128-byte cache line");

/// The shadow pass's per-instance test against an orthographic light-space
/// matrix: whether a caster at `position` with this bounding radius (already
/// scaled) can land in the shadow map. The bounding sphere is converted to NDC
/// against `shadowRadius`, the box's half-extent, and inflated by half again;
/// overshoot only costs a draw.
inline bool instanceInShadowFootprint(const glm::mat4& lightSpace, const glm::vec3& position,
                                      float boundRadiusTimesScale, float shadowRadius) {
    const glm::vec4 clip = lightSpace * glm::vec4(position, 1.0f);
    const float margin = boundRadiusTimesScale / shadowRadius * 1.5f;
    if (std::abs(clip.x) > 1.0f + margin || std::abs(clip.y) > 1.0f + margin) return false;
    return clip.z >= -margin && clip.z <= 1.0f + margin;
}

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
