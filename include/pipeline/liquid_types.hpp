#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace pipeline {

class RequiredTable;

/// The four kinds every liquid is one of: what it is to swim in, what the
/// camera sees under it, how it is drawn.
enum class LiquidBasic : uint8_t {
    Water = 0,
    Ocean = 1,
    Magma = 2,
    Slime = 3,
};

/// The LiquidType.dbc row of a basic liquid. Rows 1-4 are Water, Ocean,
/// Magma and Slime in every version of the table; the formats that only say
/// which basic kind a liquid is - MCLQ, the open terrain format - name the
/// liquid by these rows, so that every liquid anywhere carries a table id.
[[nodiscard]] uint16_t basicLiquidTypeId(LiquidBasic basic);

/// The LiquidType.dbc row a WMO group's liquid is drawn as, from the group
/// header (MOGP groupLiquid and flags) and the root header's
/// "use liquid type dbc id" flag - the client's rule. The MLIQ chunk's own
/// material id is an index into MOMT, not a liquid type.
///
/// `tileLiquid` is the low nibble of the first wet MLIQ tile, which the client
/// falls back to for a group marked green lava in a model that predates the
/// table ids. Water in an interior group (MOGP 0x2000) is "WMO Water -
/// Interior", as the client draws it.
[[nodiscard]] uint16_t wmoGroupLiquidType(uint32_t groupLiquid, uint32_t groupFlags,
                                          bool rootUsesDbcIds, uint8_t tileLiquid);

/// One row of LiquidType.dbc, as much of it as the client uses.
struct LiquidTypeInfo {
    uint32_t id = 0;
    std::string name;
    LiquidBasic basic = LiquidBasic::Water;
    /// LiquidMaterial.dbc: which shader the original client draws it with.
    uint32_t materialId = 0;
    /// LiquidType's flags; 4 marks a liquid that can be fished in.
    uint32_t flags = 0;
    /// LiquidMaterial's LVF for materialId: how a legacy MCLQ layer, which
    /// carries no vertex format of its own, lays out its heights.
    uint16_t vertexFormat = 0;
    bool materialKnown = true;
    /// The animated surface texture, a printf pattern over the frame number
    /// starting at 1 ("XTextures\\ocean\\ocean_h.%d.blp"); empty when the row
    /// has none.
    std::string texturePattern;
};

/// LiquidType.dbc: what each liquid id in the map data is.
class LiquidTypeTable {
public:
    /// Rows of the table, read through the expansion's column layout, which
    /// must name every column read. A row with a type outside the four basic
    /// kinds is reported and left out.
    static LiquidTypeTable fromDbc(const RequiredTable& liquidTypes, const RequiredTable& liquidMaterials);
    /// The four basic rows alone, without textures, for an expansion that
    /// does not use LiquidType.dbc.
    static LiquidTypeTable basicOnly();

    [[nodiscard]] const LiquidTypeInfo* find(uint32_t id) const;
    [[nodiscard]] std::size_t size() const { return rows_.size(); }
    [[nodiscard]] const std::unordered_map<uint32_t, LiquidTypeInfo>& rows() const { return rows_; }

    /// What a liquid id is, for one that may not be in the table: an id the
    /// table lacks is water, and is said once per id.
    [[nodiscard]] LiquidBasic basicOf(uint32_t id) const;

    void add(LiquidTypeInfo info);

private:
    std::unordered_map<uint32_t, LiquidTypeInfo> rows_;
    mutable std::unordered_set<uint32_t> unknownSaid_;
};

} // namespace pipeline
} // namespace wowee
