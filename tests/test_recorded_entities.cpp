#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "game/entity_controller.hpp"
#include "core/coordinates.hpp"

using namespace wowee;

TEST_CASE("recorded entities use the normal create, appearance update and despawn path", "[scene-replay][harness]") {
    test::GameHarness h;
    h.enterWorld();
    std::vector<uint32_t> displays;
    std::vector<uint64_t> removed;
    h.game().setCreatureSpawnCallback([&](uint64_t, uint32_t display, float,float,float,float,float) {
        displays.push_back(display);
    });
    h.game().setCreatureDespawnCallback([&](uint64_t guid) { removed.push_back(guid); });
    game::UpdateBlock object{};
    object.guid=0xf130000000000077ULL;
    object.objectType=game::ObjectType::UNIT;
    object.updateType=game::UpdateType::CREATE_OBJECT;
    object.hasMovement=true; object.x=5815; object.y=629; object.z=647;
    object.fields[game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID)]=1234;
    object.fields[game::fieldIndex(game::UF::UNIT_FIELD_HEALTH)]=100;
    object.fields[game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH)]=100;
    game::UpdateObjectData initial{};
    initial.blocks.push_back(object);
    h.game().getEntityController()->applyRecordedObjects(initial);
    auto entity=h.game().getEntityManager().getEntity(object.guid);
    REQUIRE(entity);
    const auto canonical=core::coords::serverToCanonical({5815,629,647});
    REQUIRE(entity->getX()==canonical.x);
    REQUIRE(entity->getY()==canonical.y);
    REQUIRE(displays==std::vector<uint32_t>{1234});

    game::UpdateObjectData changed{};
    object.updateType=game::UpdateType::VALUES;
    object.hasMovement=false;
    object.fields.clear();
    object.fields[game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID)]=5678;
    changed.blocks.push_back(object);
    h.game().getEntityController()->applyRecordedObjects(changed);
    REQUIRE(displays.back()==5678);
    entity->applySampledPosition(canonical.x+4,canonical.y+1,647,0.5f,true);
    REQUIRE(entity->isEntityMoving());
    REQUIRE(entity->getLatestX()==canonical.x+4);
    entity->applySampledPosition(canonical.x+4,canonical.y+1,647,0.5f,false);
    REQUIRE_FALSE(entity->isEntityMoving());

    game::UpdateObjectData gone{};
    gone.outOfRangeGuids.push_back(object.guid);
    h.game().getEntityController()->applyRecordedObjects(gone);
    REQUIRE_FALSE(h.game().getEntityManager().hasEntity(object.guid));
    REQUIRE(removed==std::vector<uint64_t>{object.guid});
}
