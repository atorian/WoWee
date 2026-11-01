#pragma once

#include "collision/terrain_heights.hpp"
#include "collision/world_facets.hpp"

#include <bitset>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::pipeline { class LiquidTypeTable; }

namespace wowee::collision {

/// The key a model is known by, whoever names it - an MMDX, MWMO or MODD name,
/// the path a renderer loaded: lower case, backslashes, .m2 for .mdx and .mdl.
[[nodiscard]] std::string movementModelKey(const std::string& path);

/// The map as the original 3.3.5a client collects it for a mover
/// (CMap::GetFacets): each tile with its authored MCRF chunk lists, the MDDF
/// doodads and MODF buildings those lists name, and each building's groups
/// with the MODR doodads of its doodad set. Placed from the map data, not from
/// what the renderer chose to draw, and in the original's coordinates - which
/// are the renderer's.
///
/// A placement is shared by every tile that names it, by its uniqueId, as the
/// original's map object and doodad definitions are. Models arrive as the
/// renderers build their collision shapes:
/// - an area is NotReady until every doodad model its tile names has arrived
///   (the original knows a doodad's model box before it collects it);
/// - a building whose model has not arrived, or whose group doodads have not,
///   is a placement with no groups ready, which the collector answers the way
///   the original answers a building still loading;
/// - a model that cannot be loaded is reported once, and every area naming it
///   answers Failed.
///
/// Lives beside CollisionWorld and is fed by the same calls. Read and changed
/// on the main thread only: a query marks the placements it visits.
///
/// Not verified against the original: the order buildings are listed in (the
/// order they were first placed here), the area corner flight bounds are
/// measured from, and that a building's linked groups are all its MOGI groups.
class MovementWorld final : public MovementFacetSource {
public:
    MovementWorld();
    ~MovementWorld() override;
    MovementWorld(const MovementWorld&) = delete;
    MovementWorld& operator=(const MovementWorld&) = delete;

    /// A map streamed from ADT tiles. `tilesWithTerrain` is the WDT's MAIN
    /// bit 1 per tile, [tileY * 64 + tileX]. Everything of the last map goes.
    void beginMap(uint32_t mapId, const std::bitset<64 * 64>& tilesWithTerrain);
    /// A map that is one building, the WDT's own MODF: no terrain is collected.
    /// Its bounds are its model's, placed, once the model arrives; until then
    /// it covers the map, and the map is still loading wherever it is asked.
    void beginBuildingMap(uint32_t mapId, const std::string& model, const glm::mat4& transform,
                          uint16_t doodadSet);
    [[nodiscard]] bool hasMap() const { return hasMap_; }

    void addTile(std::shared_ptr<const TerrainTileHeights> tile);
    void removeTile(int tileX, int tileY);

    /// What a liquid id is, for collecting liquid. Without it a liquid that
    /// needs its material fails, and says so once.
    void setLiquidTypes(const pipeline::LiquidTypeTable* table);

    /// A doodad model's collision, as the renderer built it: its authored
    /// collision mesh (none for a model with no collision triangles) and the
    /// header's bounding box.
    void addM2Model(const std::string& path, std::shared_ptr<const M2Shape> shape,
                    const glm::vec3& boundsMin, const glm::vec3& boundsMax);
    /// A building's collision; its path, doodads and doodad sets are set.
    void addWmoModel(std::shared_ptr<const WmoShape> shape);
    /// A model that will not arrive: logged once, with why.
    void modelUnavailable(const std::string& path, const std::string& reason);
    /// Doodad models a placement names that have not arrived, each once:
    /// for whoever can load their collision when no renderer does.
    [[nodiscard]] std::vector<std::string> takeWantedModels();

    MovementGeometryStatus query(const MovementFacetRequest& request, uint32_t flags,
                                 std::vector<MovementFacet>& output) override;

    /// What keeps the map within `radius` of `at` from being ready, for the
    /// log: a tile not streamed, a doodad model or a building not arrived.
    [[nodiscard]] std::string waitingFor(const glm::vec3& at, float radius) const;

private:
    struct DoodadEntry;
    struct BuildingEntry;
    struct Tile;
    class LiquidMaterials;

    void reset(uint32_t mapId);
    DoodadEntry* acquireDoodad(uint64_t key, const std::string& model, const glm::mat4& transform);
    BuildingEntry* acquireBuilding(uint64_t key, const std::string& model, const glm::mat4& transform,
                                   std::optional<WmoGroupBounds> bounds, uint16_t doodadSet);
    void releaseDoodad(uint64_t key);
    void releaseBuilding(uint64_t key);
    void prepareDoodad(DoodadEntry& doodad);
    bool prepareBuilding(BuildingEntry& building);
    void rebuild();

    uint32_t mapId_ = 0;
    bool hasMap_ = false;
    bool dungeon_ = false;
    std::bitset<64 * 64> tilesWithTerrain_;
    std::unordered_map<int, std::unique_ptr<Tile>> tiles_;
    std::unordered_map<uint64_t, std::unique_ptr<DoodadEntry>> doodads_;
    std::unordered_map<uint64_t, std::unique_ptr<BuildingEntry>> buildings_;
    std::vector<BuildingEntry*> buildingOrder_;
    uint64_t nextBuildingSerial_ = 1;

    struct M2Model {
        std::shared_ptr<const M2Shape> shape;
        DoodadFacetModel facets;
    };
    std::unordered_map<std::string, std::unique_ptr<M2Model>> m2Models_;
    std::unordered_map<std::string, std::shared_ptr<const WmoShape>> wmoModels_;
    std::unordered_map<std::string, std::string> unavailable_;
    std::unordered_set<std::string> asked_;
    std::vector<std::string> wanted_;

    std::unique_ptr<LiquidMaterials> materials_;
    std::unique_ptr<WorldFacetView> view_;
    std::vector<WmoWorldPlacement> placements_;
    std::unique_ptr<WorldMovementFacets> source_;
    uint32_t queryTag_ = 0;
    bool dirty_ = true;
};

}  // namespace wowee::collision
