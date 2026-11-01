#pragma once
#include "collision/map_facets.hpp"
#include "collision/terrain_chunk_facets.hpp"
#include "collision/wmo_world_facets.hpp"
#include "collision/doodad_facets.hpp"
#include "collision/entity_facets.hpp"
#include <array>
#include <unordered_map>

namespace wowee::collision {
struct WorldFacetLinks {
    std::span<DoodadFacetPlacement* const> doodads;
    std::span<EntityFacetPlacement* const> entities;
};
struct WorldFacetArea {
    bool hasTerrain = false; // WDT presence, independent of resident state.
    MovementGeometryStatus state = MovementGeometryStatus::NotReady;
    const TerrainTileHeights* terrain = nullptr;
    glm::vec3 flightCorner{};
    std::span<const WorldFacetLinks> chunks; // Authored [y*16+x] links.
};
struct WorldFacetView {
    std::array<WorldFacetArea,64*64> areas;
    std::span<const WmoWorldPlacement> buildings;
    std::span<const DestructibleCollisionProxy> proxies;
    std::unordered_map<uint64_t,WorldFacetLinks> buildingGroups;
};
// Full collection over collision-owned original-coordinate state. The owner
// retains all referenced objects, source order and streaming readiness; visual
// visibility is not an input. Queries mutate only the shared object visit tags.
// Failure must be reported once by the owner; invalid state is never treated as
// empty ready geometry. This supplied-state collector is not a streaming owner.
class WorldMovementFacets final:public MapMovementFacetSource,private WmoLinkedFacets {
public:
    WorldMovementFacets(const WorldFacetView& world,uint32_t mapId,bool dungeon,
        const TerrainLiquidMaterials& terrainMaterials,const WmoLiquidMaterials& wmoMaterials,
        EntityFacetProvider* entities,FacetNormalization normalization,uint32_t initialQueryTag=0);
    uint32_t queryTag() const { return queryTag_; }
    uint32_t originalStatusFlags() const { return statusFlags_; }
    uint32_t degenerateFaces() const { return degenerateFaces_; }
private:
    MovementGeometryStatus queryMapObjects(const MovementFacetRequest&,uint32_t,std::vector<MovementFacet>&) override;
    MovementGeometryStatus queryChunk(int,int,TerrainFacetCells,const MovementFacetRequest&,uint32_t,std::vector<MovementFacet>&) override;
    MovementGeometryStatus queryFlightBounds(int,int,const MovementFacetRequest&,std::vector<MovementFacet>&) override;
    MovementGeometryStatus doodads(uint64_t,const MovementFacetRequest&,uint32_t,std::vector<MovementFacet>&) override;
    MovementGeometryStatus destructibles(uint64_t,const MovementFacetRequest&,uint32_t,std::vector<MovementFacet>&) override;
    void appendLinks(const WorldFacetLinks&,const MovementFacetRequest&,uint32_t,std::vector<MovementFacet>&);
    const WorldFacetView& world_;
    uint32_t mapId_,queryTag_,statusFlags_=0,degenerateFaces_=0;
    const TerrainLiquidMaterials& terrainMaterials_;
    const WmoLiquidMaterials& wmoMaterials_;
    EntityFacetProvider* entities_;
    FacetNormalization normalization_;
};
} // namespace wowee::collision
