#include "collision/movement_world.hpp"

#include "collision/doodad_facets.hpp"
#include "pipeline/coordinates.hpp"
#include "foundation/logger.hpp"
#include "pipeline/liquid_types.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/placement_transform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <utility>

namespace wowee::collision {

std::string movementModelKey(const std::string& path) {
    std::string key = pipeline::modelPathToM2(path);
    for (char& c : key) {
        c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

namespace {

constexpr uint32_t kPrepared = 0x80;

/// What a building that has not arrived is to the collector: present, with no
/// groups ready. Never read beyond that.
const WmoShape kPendingBuilding{};

int tileKey(int tileX, int tileY) { return tileY * 64 + tileX; }

/// A placement's key: its uniqueId, shared by every tile naming it, or - for
/// one without - its own slot in the tile that holds it.
uint64_t placementKey(uint32_t uniqueId, int tile, size_t index) {
    if (uniqueId != 0) return uniqueId;
    return (uint64_t{1} << 63) | (static_cast<uint64_t>(tile) << 24) | index;
}

WmoGroupBounds extentsBox(const float lower[3], const float upper[3]) {
    const glm::vec3 a = core::coords::adtToWorld(lower[0], lower[1], lower[2]);
    const glm::vec3 b = core::coords::adtToWorld(upper[0], upper[1], upper[2]);
    return {glm::min(a, b), glm::max(a, b)};
}

bool inSet(const WmoDoodadSetRange& set, uint32_t index) {
    return index >= set.start && index - set.start < set.count;
}

}  // namespace

class MovementWorld::LiquidMaterials final : public TerrainLiquidMaterials, public WmoLiquidMaterials {
public:
    const pipeline::LiquidTypeTable* table = nullptr;

    std::optional<TerrainLiquidMaterial> find(uint16_t liquidType) const override {
        const auto* info = row(liquidType);
        if (!info) return std::nullopt;
        if (!info->materialKnown) {
            sayOnce(liquidType, "its material is not in LiquidMaterial.dbc");
            return std::nullopt;
        }
        return TerrainLiquidMaterial{info->flags, info->vertexFormat};
    }
    std::optional<uint32_t> flags(uint16_t liquidType) const override {
        const auto* info = row(liquidType);
        if (!info) return std::nullopt;
        return info->flags;
    }

private:
    const pipeline::LiquidTypeInfo* row(uint16_t liquidType) const {
        if (!table) {
            sayOnce(liquidType, "no liquid table is loaded");
            return nullptr;
        }
        const auto* info = table->find(liquidType);
        if (!info) sayOnce(liquidType, "LiquidType.dbc has no such row");
        return info;
    }
    void sayOnce(uint16_t liquidType, const char* why) const {
        if (!said_.insert(liquidType).second) return;
        LOG_ERROR("Movement collision: liquid type ", liquidType, " cannot be collected - ", why,
                  "; a mover asking for it is answered that the area failed");
    }
    mutable std::unordered_set<uint16_t> said_;
};

struct MovementWorld::DoodadEntry {
    DoodadFacetPlacement placement;
    std::string model;
    /// The cache's answer for the model: its collision once built, or why it
    /// cannot be had.
    std::shared_ptr<const M2Collision> collision;
    DoodadFacetModel facets;
    std::string unavailable;
    int refs = 0;
};

struct MovementWorld::BuildingEntry {
    std::string model;
    glm::mat4 transform{1.0f};
    glm::mat4 inverse{1.0f};
    /// MODF's extents, or the model's box placed once it arrives.
    std::optional<WmoGroupBounds> bounds;
    uint16_t doodadSet = 0;
    uint64_t serial = 0;
    int refs = 0;
    /// Built for the model that arrived: one entry per MOGI group, and the
    /// doodads of the placement's set each group's MODR names.
    const WmoShape* builtFor = nullptr;
    std::vector<WmoWorldGroup> groups;
    std::vector<std::unique_ptr<DoodadEntry>> groupDoodadStorage;
    std::vector<std::vector<DoodadFacetPlacement*>> groupDoodads;
    bool ready = false;
    bool failed = false;
};

struct MovementWorld::Tile {
    std::shared_ptr<const TerrainTileHeights> heights;
    std::vector<uint64_t> doodadKeys;
    std::vector<uint64_t> buildingKeys;
    std::array<std::vector<DoodadFacetPlacement*>, 256> chunkDoodads;
    std::array<WorldFacetLinks, 256> chunkLinks{};
};

MovementWorld::MovementWorld(ModelCollisionCache& models)
    : models_(models), materials_(std::make_unique<LiquidMaterials>()),
      view_(std::make_unique<WorldFacetView>()) {}

MovementWorld::~MovementWorld() = default;

void MovementWorld::reset(uint32_t mapId) {
    source_.reset();
    tiles_.clear();
    buildingOrder_.clear();
    buildings_.clear();
    doodads_.clear();
    placements_.clear();
    *view_ = WorldFacetView{};
    mapId_ = mapId;
    queryTag_ = 0;
    dirty_ = true;
}

void MovementWorld::beginMap(uint32_t mapId, const std::bitset<64 * 64>& tilesWithTerrain) {
    reset(mapId);
    hasMap_ = true;
    dungeon_ = false;
    tilesWithTerrain_ = tilesWithTerrain;
}

void MovementWorld::beginBuildingMap(uint32_t mapId, const std::string& model,
                                     const glm::mat4& transform, uint16_t doodadSet) {
    reset(mapId);
    hasMap_ = true;
    dungeon_ = true;
    tilesWithTerrain_.reset();
    acquireBuilding(placementKey(0, -1, 0), movementModelKey(model), transform, std::nullopt, doodadSet);
}

void MovementWorld::setLiquidTypes(const pipeline::LiquidTypeTable* table) {
    materials_->table = table;
}

MovementWorld::DoodadEntry* MovementWorld::acquireDoodad(uint64_t key, const std::string& model,
                                                         const glm::mat4& transform) {
    auto& slot = doodads_[key];
    if (!slot) {
        slot = std::make_unique<DoodadEntry>();
        slot->model = model;
        slot->placement.transform = transform;
        prepareDoodad(*slot);
    }
    ++slot->refs;
    return slot.get();
}

MovementWorld::BuildingEntry* MovementWorld::acquireBuilding(uint64_t key, const std::string& model,
                                                             const glm::mat4& transform,
                                                             std::optional<WmoGroupBounds> bounds,
                                                             uint16_t doodadSet) {
    auto& slot = buildings_[key];
    if (!slot) {
        slot = std::make_unique<BuildingEntry>();
        slot->model = model;
        slot->transform = transform;
        slot->inverse = glm::inverse(transform);
        slot->bounds = bounds;
        slot->doodadSet = doodadSet;
        slot->serial = nextBuildingSerial_++;
        buildingOrder_.push_back(slot.get());
    }
    ++slot->refs;
    return slot.get();
}

void MovementWorld::releaseDoodad(uint64_t key) {
    const auto found = doodads_.find(key);
    if (found != doodads_.end() && --found->second->refs <= 0) doodads_.erase(found);
}

void MovementWorld::releaseBuilding(uint64_t key) {
    const auto found = buildings_.find(key);
    if (found == buildings_.end() || --found->second->refs > 0) return;
    std::erase(buildingOrder_, found->second.get());
    buildings_.erase(found);
}

void MovementWorld::prepareDoodad(DoodadEntry& doodad) {
    auto& placement = doodad.placement;
    if (doodad.collision) return;
    auto found = models_.find(doodad.model);
    doodad.unavailable = std::move(found.unavailable);
    if (!found.collision) {
        placement.model = nullptr;
        placement.flags &= ~kPrepared;
        return;
    }
    doodad.collision = std::move(found.collision);
    doodad.facets = DoodadFacetModel{doodad.collision->boundsMin, doodad.collision->boundsMax,
                                     doodad.collision->shape.get()};
    placement.model = &doodad.facets;
    placement.flags |= kPrepared;
    transformMovementBox(placement.transform, placement.model->boundsMin, placement.model->boundsMax,
                         placement.boundsMin, placement.boundsMax);
}

bool MovementWorld::prepareBuilding(BuildingEntry& building) {
    building.ready = false;
    building.failed = unavailableBuildings_.contains(building.model);
    if (building.failed) return false;
    const auto found = wmoModels_.find(building.model);
    if (found == wmoModels_.end()) return false;
    const WmoShape& shape = *found->second;
    if (!building.bounds) {
        glm::vec3 low, high;
        transformMovementBox(building.transform, shape.boundingBoxMin, shape.boundingBoxMax, low, high);
        building.bounds = WmoGroupBounds{low, high};
    }
    if (building.builtFor != &shape) {
        building.builtFor = &shape;
        building.groups.clear();
        building.groupDoodadStorage.clear();
        building.groupDoodads.assign(shape.originalGroups.size(), {});
        const WmoDoodadSetRange* chosen =
            building.doodadSet < shape.doodadSets.size() ? &shape.doodadSets[building.doodadSet] : nullptr;
        for (size_t index = 0; index < shape.originalGroups.size(); ++index) {
            const auto& group = shape.originalGroups[index];
            if (!group.bounds) {
                buildingUnavailable(building.model, "group " + std::to_string(index) + " has no MOGI bounds");
                building.failed = true;
                return false;
            }
            WmoWorldGroup linked;
            linked.index = static_cast<uint32_t>(index);
            transformMovementBox(building.transform, group.bounds->low, group.bounds->high,
                                 linked.bounds.low, linked.bounds.high);
            linked.link = (building.serial << 16) | index;
            building.groups.push_back(linked);
            if (!group.shape || !group.shape->original) continue;
            for (const uint16_t reference : group.shape->original->doodadReferences) {
                if (reference >= shape.doodads.size()) continue;
                const bool active = (!shape.doodadSets.empty() && inSet(shape.doodadSets[0], reference)) ||
                                    (chosen && inSet(*chosen, reference));
                const auto& def = shape.doodads[reference];
                if (!active || def.model.empty()) continue;
                auto entry = std::make_unique<DoodadEntry>();
                entry->model = def.model;
                entry->placement.transform = building.transform * def.transform;
                building.groupDoodads[index].push_back(&entry->placement);
                building.groupDoodadStorage.push_back(std::move(entry));
            }
        }
    }
    for (auto& doodad : building.groupDoodadStorage) {
        prepareDoodad(*doodad);
        if (!doodad->unavailable.empty()) {
            building.failed = true;
            return false;
        }
        if (!doodad->placement.model) return false;
    }
    building.ready = true;
    return true;
}

void MovementWorld::addTile(std::shared_ptr<const TerrainTileHeights> heights) {
    if (!heights) return;
    const int key = tileKey(heights->tileX, heights->tileY);
    if (tiles_.contains(key)) removeTile(heights->tileX, heights->tileY);
    auto tile = std::make_unique<Tile>();
    tile->heights = heights;
    tile->doodadKeys.assign(heights->doodadPlacements.size(), 0);
    for (size_t index = 0; index < heights->doodadPlacements.size(); ++index) {
        const auto& placement = heights->doodadPlacements[index];
        if (placement.nameId >= heights->doodadModels.size()) {
            LOG_WARNING("Movement collision: tile [", heights->tileX, ",", heights->tileY, "] doodad ", index,
                        " names model ", placement.nameId, " of ", heights->doodadModels.size(),
                        " - it is not solid");
            continue;
        }
        const uint64_t doodadKey = placementKey(placement.uniqueId, key, index);
        acquireDoodad(doodadKey, heights->doodadModels[placement.nameId],
                      pipeline::adtPlacementMatrix(placement.position, placement.rotation,
                                                   placement.scale / 1024.0f));
        tile->doodadKeys[index] = doodadKey;
    }
    tile->buildingKeys.assign(heights->wmoPlacements.size(), 0);
    for (size_t index = 0; index < heights->wmoPlacements.size(); ++index) {
        const auto& placement = heights->wmoPlacements[index];
        if (placement.nameId >= heights->wmoModels.size()) {
            LOG_WARNING("Movement collision: tile [", heights->tileX, ",", heights->tileY, "] building ", index,
                        " names model ", placement.nameId, " of ", heights->wmoModels.size(),
                        " - it is not solid");
            continue;
        }
        const uint64_t buildingKey = placementKey(placement.uniqueId, key, index);
        acquireBuilding(buildingKey, heights->wmoModels[placement.nameId],
                        pipeline::adtPlacementMatrix(placement.position, placement.rotation,
                                                     placement.scale / 1024.0f),
                        extentsBox(placement.extentLower, placement.extentUpper), placement.doodadSet);
        tile->buildingKeys[index] = buildingKey;
    }
    for (size_t chunk = 0; chunk < heights->chunks.size(); ++chunk) {
        auto& list = tile->chunkDoodads[chunk];
        for (const uint32_t reference : heights->chunks[chunk].doodadReferences) {
            if (reference < tile->doodadKeys.size() && tile->doodadKeys[reference] != 0) {
                list.push_back(&doodads_.at(tile->doodadKeys[reference])->placement);
            }
        }
        tile->chunkLinks[chunk] = WorldFacetLinks{list, {}};
    }
    tiles_[key] = std::move(tile);
    dirty_ = true;
}

void MovementWorld::removeTile(int tileX, int tileY) {
    const auto found = tiles_.find(tileKey(tileX, tileY));
    if (found == tiles_.end()) return;
    const auto tile = std::move(found->second);
    tiles_.erase(found);
    for (const uint64_t key : tile->doodadKeys) {
        if (key != 0) releaseDoodad(key);
    }
    for (const uint64_t key : tile->buildingKeys) {
        if (key != 0) releaseBuilding(key);
    }
    dirty_ = true;
}

void MovementWorld::addWmoModel(std::shared_ptr<const WmoShape> shape) {
    if (!shape) return;
    if (shape->path.empty()) {
        LOG_ERROR("Movement collision: a building's collision arrived with no path - "
                  "no placement can name it, so it is not solid");
        return;
    }
    wmoModels_[shape->path] = std::move(shape);
    dirty_ = true;
}

void MovementWorld::buildingUnavailable(const std::string& path, const std::string& reason) {
    const std::string key = movementModelKey(path);
    if (!unavailableBuildings_.emplace(key, reason).second) return;
    LOG_ERROR("Movement collision: building ", key, " is unavailable (", reason,
              ") - an area that places it cannot be collided with");
    dirty_ = true;
}

void MovementWorld::rebuild() {
    if (source_) queryTag_ = source_->queryTag();
    source_.reset();
    modelsSeen_ = models_.revision();
    for (auto& [key, doodad] : doodads_) prepareDoodad(*doodad);
    for (auto* building : buildingOrder_) prepareBuilding(*building);

    for (auto& area : view_->areas) area = WorldFacetArea{};
    for (size_t index = 0; index < view_->areas.size(); ++index) {
        view_->areas[index].hasTerrain = tilesWithTerrain_[index];
    }
    for (const auto& [key, tile] : tiles_) {
        auto& area = view_->areas[static_cast<size_t>(key)];
        const auto& heights = *tile->heights;
        area.terrain = &heights;
        area.chunks = tile->chunkLinks;
        area.flightCorner = glm::vec3(heights.chunks[0].originalCorner.x, heights.chunks[0].originalCorner.y, 0.0f);
        auto state = heights.collisionDataValid ? MovementGeometryStatus::Ready : MovementGeometryStatus::Failed;
        for (const uint64_t doodadKey : tile->doodadKeys) {
            if (state == MovementGeometryStatus::Failed || doodadKey == 0) continue;
            const auto& doodad = *doodads_.at(doodadKey);
            if (!doodad.unavailable.empty()) state = MovementGeometryStatus::Failed;
            else if (!doodad.placement.model) state = MovementGeometryStatus::NotReady;
        }
        for (const uint64_t buildingKey : tile->buildingKeys) {
            if (buildingKey != 0 && buildings_.at(buildingKey)->failed) state = MovementGeometryStatus::Failed;
        }
        area.state = state;
    }

    placements_.clear();
    view_->buildingGroups.clear();
    for (const auto* building : buildingOrder_) {
        WmoWorldPlacement placement;
        const auto shape = wmoModels_.find(building->model);
        placement.model = shape != wmoModels_.end() ? shape->second.get() : &kPendingBuilding;
        placement.groupsReady = building->ready;
        placement.bounds = building->bounds.value_or(WmoGroupBounds{
            glm::vec3(-std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::max())});
        placement.transform = building->transform;
        placement.inverse = building->inverse;
        if (building->ready) {
            placement.groups = building->groups;
            for (size_t group = 0; group < building->groups.size(); ++group) {
                view_->buildingGroups.emplace(building->groups[group].link,
                                              WorldFacetLinks{building->groupDoodads[group], {}});
            }
        }
        placements_.push_back(placement);
    }
    view_->buildings = placements_;
    source_ = std::make_unique<WorldMovementFacets>(*view_, mapId_, dungeon_, *materials_, *materials_,
                                                    nullptr, FacetNormalization::X87, queryTag_);
    dirty_ = false;
}

std::string MovementWorld::waitingFor(const glm::vec3& at, float radius) const {
    std::string what;
    const auto add = [&what](const std::string& item) {
        if (!what.empty()) what += "; ";
        what += item;
    };
    const auto [lowX, lowY] = core::coords::worldToTile(at.x + radius, at.y + radius);
    const auto [highX, highY] = core::coords::worldToTile(at.x - radius, at.y - radius);
    for (int tileY = std::min(lowY, highY); tileY <= std::max(lowY, highY); ++tileY) {
        for (int tileX = std::min(lowX, highX); tileX <= std::max(lowX, highX); ++tileX) {
            const int key = tileKey(tileX, tileY);
            const auto tile = tiles_.find(key);
            if (tile == tiles_.end()) {
                if (tilesWithTerrain_[static_cast<size_t>(key)]) {
                    add("tile [" + std::to_string(tileX) + "," + std::to_string(tileY) + "] is not loaded");
                }
                continue;
            }
            for (const uint64_t doodadKey : tile->second->doodadKeys) {
                if (doodadKey == 0) continue;
                const auto& doodad = *doodads_.at(doodadKey);
                if (!doodad.placement.model && doodad.unavailable.empty()) {
                    add("tile [" + std::to_string(tileX) + "," + std::to_string(tileY) + "] doodad model " +
                        doodad.model + " has not arrived");
                    break;
                }
            }
        }
    }
    const glm::vec3 low = at - glm::vec3(radius), high = at + glm::vec3(radius);
    for (const auto* building : buildingOrder_) {
        if (building->ready) continue;
        if (building->bounds && (high.x < building->bounds->low.x || high.y < building->bounds->low.y ||
                                 high.z < building->bounds->low.z || low.x > building->bounds->high.x ||
                                 low.y > building->bounds->high.y || low.z > building->bounds->high.z)) {
            continue;
        }
        if (!wmoModels_.contains(building->model)) {
            add("building " + building->model + " has not arrived");
            continue;
        }
        for (const auto& doodad : building->groupDoodadStorage) {
            if (!doodad->placement.model) {
                add("building " + building->model + " doodad model " + doodad->model + " has not arrived");
                break;
            }
        }
    }
    return what.empty() ? "nothing named - a request outside the loaded map" : what;
}

MovementGeometryStatus MovementWorld::query(const MovementFacetRequest& request, uint32_t flags,
                                            std::vector<MovementFacet>& output) {
    if (!hasMap_) {
        output.clear();
        return MovementGeometryStatus::NotReady;
    }
    if (dirty_ || models_.revision() != modelsSeen_) rebuild();
    return source_->query(request, flags, output);
}

}  // namespace wowee::collision
