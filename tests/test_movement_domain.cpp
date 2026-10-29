// Movement, end to end: what the server says of where things are and go,
// and what the rest of the client is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/entity.hpp"
#include "game/game_events.hpp"

#include <memory>
#include <vector>

using wowee::test::GameHarness;
namespace ev = wowee::game::events;

TEST_CASE("following stops on the next frame once the followed player leaves view", "[movement][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    const auto& me = h.game().getMovementInfo();
    auto friendPlayer = std::make_shared<Player>(0xB001);
    friendPlayer->setName("Anna");
    friendPlayer->setPosition(me.x + 3.0f, me.y, me.z, 0.0f);
    h.game().getEntityManager().addEntity(friendPlayer->getGuid(), friendPlayer);
    std::vector<bool> following;
    auto sub = h.events().subscribe<ev::AutoFollowChanged>(
        [&](const ev::AutoFollowChanged& e) { following.push_back(e.renderPos != nullptr); });

    h.game().combat().setTarget(0xB001);
    h.game().movement().followTarget();
    h.pump();
    REQUIRE(following == std::vector<bool>{true});
    REQUIRE(h.addonEvents("AUTOFOLLOW_BEGIN").size() == 1);
    CHECK(h.addonEvents("AUTOFOLLOW_BEGIN")[0].args.at(0) == "Anna");

    h.game().getEntityManager().removeEntity(0xB001);
    h.pump();
    CHECK(following == std::vector<bool>{true, false});
    CHECK(h.addonEvents("AUTOFOLLOW_END").size() == 1);
}
