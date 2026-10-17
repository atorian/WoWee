#include <catch_amalgamated.hpp>

#include "game/zone_manager.hpp"

TEST_CASE("Duskwood ADT coverage follows client map bounds", "[zone_tiles]") {
    using wowee::game::isDuskwoodAdtTile;

    REQUIRE(isDuskwoodAdtTile(33, 52));
    REQUIRE(isDuskwoodAdtTile(35, 53));
    REQUIRE(isDuskwoodAdtTile(52, 33)); // Transposed extracted-map convention
    REQUIRE_FALSE(isDuskwoodAdtTile(34, 51)); // Shared Elwynn/Duskwood bank ADT
    REQUIRE_FALSE(isDuskwoodAdtTile(32, 52));
    REQUIRE_FALSE(isDuskwoodAdtTile(36, 52));
}
