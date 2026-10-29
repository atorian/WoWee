#include "rendering/wmo_minimap.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace wowee::rendering {

namespace {

std::string tileStemOf(const std::string& path) {
    std::string stem = path;
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const char* prefix : {"world\\", "world/"}) {
        if (lower.rfind(prefix, 0) == 0) {
            stem.erase(0, 6);
            lower.erase(0, 6);
            break;
        }
    }
    if (lower.size() > 4 && lower.ends_with(".wmo")) stem.resize(stem.size() - 4);
    std::replace(stem.begin(), stem.end(), '/', '\\');
    return stem;
}

bool holds(const WmoMinimapModel::Group& g, const glm::vec3& p) {
    return p.x >= g.boundsMin.x && p.x <= g.boundsMax.x && p.y >= g.boundsMin.y &&
           p.y <= g.boundsMax.y && p.z >= g.boundsMin.z && p.z <= g.boundsMax.z;
}

/// Which faces of `box` a point lies outside of, one bit each (0x71cad0).
uint32_t outcode(const glm::vec3& p, const WmoMinimapBox& box) {
    uint32_t code = 0;
    if (p.x < box.min.x) code |= 0x01;
    if (p.y < box.min.y) code |= 0x02;
    if (p.z < box.min.z) code |= 0x04;
    if (p.x > box.max.x) code |= 0x08;
    if (p.y > box.max.y) code |= 0x10;
    if (p.z > box.max.z) code |= 0x20;
    return code;
}

/// Whether a group's box meets the gather box seen from above (0x71cb70):
/// height plays no part here, only in which portals lead on.
bool meetsFromAbove(const WmoMinimapModel::Group& g, const WmoMinimapBox& box) {
    return !(g.boundsMax.x < box.min.x || g.boundsMax.y < box.min.y ||
             g.boundsMin.x > box.max.x || g.boundsMin.y > box.max.y);
}

constexpr size_t kMaxTiles = 0x200;

struct Walk {
    const WmoMinimapModel& model;
    const WmoMinimapBox& box;
    bool indoors;
    std::vector<bool> visited;
    std::vector<WmoMinimapTile> tiles;

    void cut(int index) {
        const auto& g = model.groups[index];
        if (g.flags & kWmoGroupUnreachable) return;
        const int columns = static_cast<int>(std::ceil((g.boundsMax.x - g.boundsMin.x) / kWmoMinimapTileYards));
        const int rows = static_cast<int>(std::ceil((g.boundsMax.y - g.boundsMin.y) / kWmoMinimapTileYards));
        constexpr float kEdge = 1.0f;
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                const glm::vec2 origin(g.boundsMin.x + column * kWmoMinimapTileYards,
                                       g.boundsMin.y + row * kWmoMinimapTileYards);
                glm::vec2 lo = origin, hi = origin + glm::vec2(kWmoMinimapTileYards);
                if (column == 0) lo.x -= kEdge;
                if (row == 0) lo.y -= kEdge;
                if (column == columns - 1) hi.x += kEdge;
                if (row == rows - 1) hi.y += kEdge;
                if (hi.x < box.min.x || hi.y < box.min.y || lo.x > box.max.x || lo.y > box.max.y) continue;
                if (tiles.size() >= kMaxTiles) continue;
                tiles.push_back({.group = index, .x = column, .y = row, .origin = origin,
                                 .midZ = (g.boundsMin.z + g.boundsMax.z) * 0.5f});
            }
        }
    }

    void visit(int index, int from) {
        if (index < 0 || static_cast<size_t>(index) >= model.groups.size()) return;
        const auto& g = model.groups[index];
        if (indoors && (g.flags & kWmoGroupExterior)) return;
        if (visited[index]) return;
        visited[index] = true;
        if (!meetsFromAbove(g, box)) return;
        cut(index);
        for (uint32_t r = g.firstPortalRef; r < uint32_t(g.firstPortalRef) + g.portalRefCount; ++r) {
            if (r >= model.portalRefs.size()) break;
            const auto& ref = model.portalRefs[r];
            if (ref.group == from || ref.portal >= model.portals.size()) continue;
            const auto& portal = model.portals[ref.portal];
            uint32_t outside = ~0u;
            for (uint32_t v = portal.firstVertex; v < uint32_t(portal.firstVertex) + portal.vertexCount; ++v) {
                if (v >= model.portalVertices.size()) break;
                outside &= outcode(model.portalVertices[v], box);
            }
            if (outside == 0) visit(ref.group, index);
        }
    }
};

}  // namespace

std::shared_ptr<const WmoMinimapModel> WmoMinimapModel::from(const pipeline::WMOModel& wmo) {
    if (wmo.sourcePath.empty()) return nullptr;
    auto model = std::make_shared<WmoMinimapModel>();
    model->tileStem = tileStemOf(wmo.sourcePath);
    model->wmoId = wmo.wmoId;
    model->groups.reserve(wmo.groups.size());
    for (const auto& group : wmo.groups) {
        model->groups.push_back({.boundsMin = group.boundingBoxMin, .boundsMax = group.boundingBoxMax,
                                 .flags = group.flags, .firstPortalRef = group.portalStart,
                                 .portalRefCount = group.portalCount});
    }
    for (const auto& ref : wmo.portalRefs) model->portalRefs.push_back({ref.portalIndex, ref.groupIndex});
    for (const auto& portal : wmo.portals) model->portals.push_back({portal.startVertex, portal.vertexCount});
    model->portalVertices = wmo.portalVertices;
    return model;
}

int WmoMinimapModel::groupAt(const glm::vec3& local) const {
    int best = -1;
    bool bestInterior = false;
    float bestVolume = std::numeric_limits<float>::max();
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (!holds(g, local)) continue;
        const bool interior = (g.flags & kWmoGroupInterior) != 0;
        const glm::vec3 size = g.boundsMax - g.boundsMin;
        const float volume = size.x * size.y * size.z;
        if (best < 0 || (interior && !bestInterior) || (interior == bestInterior && volume < bestVolume)) {
            best = static_cast<int>(i);
            bestInterior = interior;
            bestVolume = volume;
        }
    }
    return best;
}

WmoMinimapBox wmoMinimapGatherBox(const glm::vec3& world, float radius) {
    const glm::vec2 cell(std::floor(world.x / radius) * radius, std::floor(world.y / radius) * radius);
    return {.min = glm::vec3(cell - glm::vec2(radius), world.z - 1.5f * radius),
            .max = glm::vec3(cell + glm::vec2(2.0f * radius), world.z + radius)};
}

WmoMinimapBox wmoMinimapModelBox(const WmoMinimapBox& world, const glm::mat4& worldToModel) {
    WmoMinimapBox out{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(-std::numeric_limits<float>::max())};
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? world.max.x : world.min.x, (i & 2) ? world.max.y : world.min.y,
                               (i & 4) ? world.max.z : world.min.z);
        const glm::vec3 p = glm::vec3(worldToModel * glm::vec4(corner, 1.0f));
        out.min = glm::min(out.min, p);
        out.max = glm::max(out.max, p);
    }
    return out;
}

std::vector<WmoMinimapTile> wmoMinimapTiles(const WmoMinimapModel& model, int startGroup, bool indoors,
                                            WmoMinimapBox box) {
    if (startGroup < 0 || static_cast<size_t>(startGroup) >= model.groups.size()) return {};
    // Nothing above the top of the player's own group (0x71e7c7).
    box.max.z = model.groups[startGroup].boundsMax.z;
    Walk walk{.model = model, .box = box, .indoors = indoors,
              .visited = std::vector<bool>(model.groups.size(), false), .tiles = {}};
    walk.visit(startGroup, startGroup);
    return std::move(walk.tiles);
}

void wmoMinimapDrawOrder(std::vector<WmoMinimapTile>& tiles, int playerGroup, float playerModelZ) {
    const auto key = [&](const WmoMinimapTile& t) {
        return t.group == playerGroup ? std::numeric_limits<float>::max() : t.midZ - playerModelZ;
    };
    std::stable_sort(tiles.begin(), tiles.end(),
                     [&](const WmoMinimapTile& a, const WmoMinimapTile& b) { return key(a) < key(b); });
}

glm::vec2 wmoMinimapAreaUV(const glm::vec3& world, const WmoMinimapBox& area) {
    return {(area.max.x - world.x) / (area.max.x - area.min.x), (area.max.y - world.y) / (area.max.y - area.min.y)};
}

MinimapQuad wmoMinimapQuad(const WmoMinimapTile& tile, int imageWidth, int imageHeight, const glm::mat4& modelToWorld,
                           const WmoMinimapBox& area) {
    constexpr float kPixelsPerYard = 2.0f;
    const float wide = imageWidth / kPixelsPerYard, high = imageHeight / kPixelsPerYard;
    const auto at = [&](float x, float y) {
        return wmoMinimapAreaUV(glm::vec3(modelToWorld * glm::vec4(x, y, tile.midZ, 1.0f)), area);
    };
    const glm::vec2 topLeft = at(tile.origin.x, tile.origin.y + high);
    return {.corner = topLeft,
            .down = at(tile.origin.x, tile.origin.y) - topLeft,
            .across = at(tile.origin.x + wide, tile.origin.y + high) - topLeft};
}

std::optional<WmoAreaFlags> WmoAreaFlags::read(const pipeline::DBCFile& dbc, const pipeline::DBCFieldMap* layout) {
    uint32_t wmoId = 1, nameSet = 2, group = 3, flags = 9;
    if (layout && layout->tryField("Flags") != 0xFFFFFFFFu) {
        wmoId = layout->tryField("WMOID");
        nameSet = layout->tryField("NameSetID");
        group = layout->tryField("WMOGroupID");
        flags = layout->tryField("Flags");
    } else if (dbc.getFieldCount() != 28) {
        return std::nullopt;
    }
    const uint32_t fields = dbc.getFieldCount();
    if (wmoId >= fields || nameSet >= fields || group >= fields || flags >= fields) return std::nullopt;
    WmoAreaFlags table;
    for (uint32_t row = 0; row < dbc.getRecordCount(); ++row) {
        if (static_cast<int32_t>(dbc.getUInt32(row, group)) != -1) continue;
        table.set(dbc.getUInt32(row, wmoId), static_cast<uint16_t>(dbc.getUInt32(row, nameSet)),
                  dbc.getUInt32(row, flags));
    }
    return table;
}

uint32_t WmoAreaFlags::rootFlags(uint32_t wmoId, uint16_t nameSet) const {
    const auto found = flags_.find((uint64_t(wmoId) << 16) | nameSet);
    return found == flags_.end() ? 0 : found->second;
}

void WmoAreaFlags::set(uint32_t wmoId, uint16_t nameSet, uint32_t flags) {
    flags_[(uint64_t(wmoId) << 16) | nameSet] = flags;
}

}  // namespace wowee::rendering
