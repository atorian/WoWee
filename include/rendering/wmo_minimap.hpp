#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace wowee::pipeline { struct WMOModel; class RequiredTable; }

namespace wowee::rendering {

/// What the minimap needs of a building to draw it the way the 3.3.5a client
/// does when the player is in it: its groups, portals and the name its
/// minimap tiles are filed under.
///
/// Groups are numbered as the building's files number them, which is what the
/// tile names and the portal references count by. The renderer's own list
/// leaves out groups with nothing to draw, so it cannot be used for this.
struct WmoMinimapModel {
    struct Group {
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
        uint32_t flags = 0;
        uint16_t firstPortalRef = 0;
        uint16_t portalRefCount = 0;
    };
    struct PortalRef { uint16_t portal = 0; uint16_t group = 0; };
    struct Portal { uint16_t firstVertex = 0; uint16_t vertexCount = 0; };

    /// The tile name stem: the model path without "World\" and ".wmo"
    /// ("WMO\Northrend\Dalaran\ND_Dalaran"), to which "_GGG_XX_YY" is added.
    std::string tileStem;
    /// MOHD wmoID, which WMOAreaTable.dbc names the building by.
    uint32_t wmoId = 0;
    std::vector<Group> groups;
    std::vector<PortalRef> portalRefs;
    std::vector<Portal> portals;
    std::vector<glm::vec3> portalVertices;

    /// Null for a model that has no path to name its tiles by.
    [[nodiscard]] static std::shared_ptr<const WmoMinimapModel> from(const pipeline::WMOModel& model);

    /// The group a model-space point stands in: an interior group holding it
    /// before any exterior one, and the smallest that holds it among those -
    /// the rule the renderer's portal culling and the indoor test use. -1 when
    /// no group's box holds it.
    [[nodiscard]] int groupAt(const glm::vec3& local) const;
};

static constexpr uint32_t kWmoGroupExterior = 0x8;
static constexpr uint32_t kWmoGroupUnreachable = 0x80;
static constexpr uint32_t kWmoGroupInterior = 0x2000;

/// The minimap radius in yards at each indoor zoom level, nearest last
/// (wow.exe 3.3.5a, 0x979e84).
static constexpr std::array<float, 6> kWmoMinimapRadius = {150.0f, 120.0f, 90.0f, 60.0f, 40.0f, 25.0f};

/// The world size of one building minimap tile at full size: 128 yards
/// (0x919ff0: 0.5 * 256). A tile image is drawn at two pixels a yard, so a
/// smaller image covers less.
static constexpr float kWmoMinimapTileYards = 128.0f;

struct WmoMinimapBox {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/// The world box the client gathers building tiles over around the player
/// (0x75d81a): the radius-sized square cell the player is in, widened by the
/// radius on every side, and in height from one and a half radii below the
/// player to one radius above. The gathered tiles change only when the player
/// crosses into another cell.
[[nodiscard]] WmoMinimapBox wmoMinimapGatherBox(const glm::vec3& world, float radius);

/// A world box as the model-space box that holds it.
[[nodiscard]] WmoMinimapBox wmoMinimapModelBox(const WmoMinimapBox& world, const glm::mat4& worldToModel);

struct WmoMinimapTile {
    int group = 0;
    int x = 0;
    int y = 0;
    /// The model-space corner the tile starts at: the group's box minimum
    /// plus 128 yards per step.
    glm::vec2 origin{0.0f};
    /// The middle of the group's height span, model space.
    float midZ = 0.0f;
};

/// The tiles the 3.3.5a client draws for a player in `startGroup` (wow.exe
/// 0x71e610 and 0x71e090).
///
/// From the player's group it walks through the portals to the groups next
/// door, through every portal not wholly outside `box`, never straight back.
/// A group whose box misses `box` is not drawn or walked through, nor one
/// above the top of the player's group, nor one flagged unreachable, nor,
/// indoors, an exterior group. Each group drawn is cut into 128-yard tiles
/// from its box minimum, X then Y, and a tile is kept when it, widened by a
/// yard at the group's outer edges, meets `box`. At most 512.
[[nodiscard]] std::vector<WmoMinimapTile> wmoMinimapTiles(const WmoMinimapModel& model, int startGroup,
                                                          bool indoors, WmoMinimapBox box);

/// The order the gathered tiles are drawn in: the player's own group last,
/// over everything, and the others from the lowest up, by the middle of their
/// group's height against the player's. This is read from what the client
/// keeps per tile slot (0x75b8d6: the player's group gets the largest float,
/// any other its middle height less the player's) rather than from its
/// drawing code, which has not been traced.
void wmoMinimapDrawOrder(std::vector<WmoMinimapTile>& tiles, int playerGroup, float playerModelZ);

/// Where an image is drawn in the minimap's composite image, in that image's
/// 0..1 space: the image's top-left corner, and the composite distance its
/// left edge runs down (`down`) and its top edge across (`across`).
struct MinimapQuad {
    glm::vec2 corner{0.0f};
    glm::vec2 down{0.0f};
    glm::vec2 across{0.0f};
};

/// A world point's place in a composite that covers `area` seen from above:
/// u grows east and v south, as the terrain composite lays its tiles out.
/// Render space has x west and y north.
[[nodiscard]] glm::vec2 wmoMinimapAreaUV(const glm::vec3& world, const WmoMinimapBox& area);

/// Where a building tile image is drawn in a composite covering `area`.
/// The image is two pixels a yard; its left edge is the tile's origin X and
/// its bottom edge the origin Y, so it runs along +X and up +Y of the model
/// (checked against Dalaran's tiles: group 0 is 26.1 by 15.0 yards and its
/// 64x32 image is filled 52 pixels from the left and 30 up from the bottom).
[[nodiscard]] MinimapQuad wmoMinimapQuad(const WmoMinimapTile& tile, int imageWidth, int imageHeight,
                                         const glm::mat4& modelToWorld, const WmoMinimapBox& area);

/// A placed building as the minimap draws it.
struct WmoMinimapPlacement {
    uint32_t instance = 0;
    std::shared_ptr<const WmoMinimapModel> model;
    glm::mat4 modelToWorld{1.0f};
    glm::mat4 worldToModel{1.0f};
    /// The placement's NameSetID: which WMOAreaTable rows name it.
    uint16_t nameSet = 0;
};

/// WMOAreaTable.dbc's Flags for each building as a whole: its rows with
/// WMOGroupID -1, by WMOID and NameSetID.
class WmoAreaFlags {
public:
    /// The columns the expansion's layout names, every one of which it must.
    [[nodiscard]] static WmoAreaFlags read(const pipeline::RequiredTable& wmoAreaTable);
    /// The building row's flags; zero when the table has no such row.
    [[nodiscard]] uint32_t rootFlags(uint32_t wmoId, uint16_t nameSet) const;
    void set(uint32_t wmoId, uint16_t nameSet, uint32_t flags);

private:
    std::unordered_map<uint64_t, uint32_t> flags_;
};

/// Whether the minimap shows the building's tiles instead of the ground
/// (0x75d6c2): inside an interior group, or anywhere in a building whose
/// WMOAreaTable row has flag 0x1 - Dalaran's does, so its streets are drawn
/// from the city's own tiles and not from the forest below.
[[nodiscard]] constexpr bool wmoMinimapShown(bool indoors, uint32_t rootFlags) {
    return indoors || (rootFlags & 0x1u) != 0;
}

}  // namespace wowee::rendering
