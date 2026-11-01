#pragma once
#include "collision/movement_request.hpp"
#include "collision/terrain_facets.hpp"

namespace wowee::collision {
// GetChunkFacets' unavailable-area branch. hasTerrain comes from WDT bit 1;
// the caller uses this only while an area is absent or still loading. A missing
// chunk in an already resident area instead returns NotReady directly.
[[nodiscard]] MovementGeometryStatus appendUnavailableTerrainFacets(
    int chunkX, int chunkY, bool hasTerrain, const MovementFacetRequest& request,
    uint32_t flags, std::vector<MovementFacet>& output);

// Original map query dispatcher (CMap::GetFacets, 0x7a5f20). Geometry owners
// provide the three original collection boundaries; their order and failures
// are observable to the mover. A chunk failure does not stop later chunks.
// This base cannot be used without implementing all geometry sources.
class MapMovementFacetSource : public MovementFacetSource {
public:
    explicit MapMovementFacetSource(bool dungeon) : dungeon_(dungeon) {}
    MovementGeometryStatus query(const MovementFacetRequest& request, uint32_t flags,
                                std::vector<MovementFacet>& output) final;
protected:
    virtual MovementGeometryStatus queryMapObjects(const MovementFacetRequest& request,
        uint32_t flags, std::vector<MovementFacet>& output) = 0;
    virtual MovementGeometryStatus queryChunk(int x, int y, TerrainFacetCells cells,
        const MovementFacetRequest& request, uint32_t flags,
        std::vector<MovementFacet>& output) = 0;
    virtual MovementGeometryStatus queryFlightBounds(int x, int y,
        const MovementFacetRequest& request, std::vector<MovementFacet>& output) = 0;
private:
    bool dungeon_;
};
} // namespace wowee::collision
