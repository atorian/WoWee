// Entity, Unit, Player, GameObject, EntityManager tests
#include <catch_amalgamated.hpp>
#include "game/entity.hpp"
#include "game/movement_flags.hpp"
#include <algorithm>
#include <memory>

using namespace wowee::game;

TEST_CASE("Entity default construction", "[entity]") {
    Entity e;
    REQUIRE(e.getGuid() == 0);
    REQUIRE(e.getType() == ObjectType::OBJECT);
    REQUIRE(e.getX() == 0.0f);
    REQUIRE(e.getY() == 0.0f);
    REQUIRE(e.getZ() == 0.0f);
    REQUIRE(e.getOrientation() == 0.0f);
}

TEST_CASE("Entity GUID constructor", "[entity]") {
    Entity e(0xDEADBEEF);
    REQUIRE(e.getGuid() == 0xDEADBEEF);
}

TEST_CASE("Entity position set/get", "[entity]") {
    Entity e;
    e.setPosition(1.0f, 2.0f, 3.0f, 1.57f);
    REQUIRE(e.getX() == Catch::Approx(1.0f));
    REQUIRE(e.getY() == Catch::Approx(2.0f));
    REQUIRE(e.getZ() == Catch::Approx(3.0f));
    REQUIRE(e.getOrientation() == Catch::Approx(1.57f));
}

TEST_CASE("Entity ignores positive-duration movement without displacement",
          "[entity][movement]") {
    Entity e;
    e.setPosition(10.0f, 20.0f, 30.0f, 0.0f);

    e.startMoveTo(10.0f, 20.0f, 30.0f, 1.25f, 0.5f);

    REQUIRE_FALSE(e.isEntityMoving());
    REQUIRE(e.getOrientation() == Catch::Approx(1.25f));
}

TEST_CASE("Entity still interpolates meaningful movement", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);

    e.startMoveTo(5.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    REQUIRE(e.isEntityMoving());

    e.updateMovement(0.5f);
    REQUIRE(e.getX() == Catch::Approx(2.5f));
    REQUIRE(e.isEntityMoving());
}

TEST_CASE("Entity stops at the end of a server move", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveTo(5.0f, 0.0f, 0.0f, 0.0f, 1.0f);

    e.updateMovement(1.5f);
    REQUIRE_FALSE(e.isEntityMoving());
    REQUIRE(e.getX() == Catch::Approx(5.0f));
}

namespace {

constexpr uint32_t moveBit(MovementFlags f) { return static_cast<uint32_t>(f); }

ReportedMovement::Clock::time_point at(int milliseconds) {
    return ReportedMovement::Clock::time_point(std::chrono::milliseconds(1000000 + milliseconds));
}

MovementReport running(uint32_t moverTime, float serverX, float serverY, float orientation = 0.0f) {
    MovementReport r;
    r.moverTime = moverTime;
    r.position = {serverX, serverY, 10.0f};
    r.orientation = orientation;  // 0 faces +x on the server's axes
    r.flags = moveBit(MovementFlags::FORWARD);
    return r;
}

/// Frames of 10 ms from one moment of this client's clock to another.
void play(ReportedMovement& m, int fromMs, int toMs, const MovementSpeeds& speeds = {}) {
    for (int t = fromMs + 10; t <= toMs; t += 10) m.advance(0.01f, at(t), speeds);
}

constexpr float kHalfPi = 1.5707963f;

}  // namespace

TEST_CASE("Reported movement carries a runner on between reports", "[entity][movement]") {
    ReportedMovement m;
    m.report(running(0, 0.0f, 0.0f), at(0));
    play(m, 0, 700);

    // Playback trails the report by its delay, and covers that time at run speed.
    REQUIRE(m.isTravelling());
    REQUIRE(m.playbackTime() > 600.0);
    REQUIRE(m.playbackTime() < 700.0);
    REQUIRE(m.position().x == Catch::Approx(7.0 * m.playbackTime() / 1000.0).margin(0.01f));
    REQUIRE(m.position().y == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("Reported movement plays reports that come on time in order", "[entity][movement]") {
    ReportedMovement m;
    m.report(running(0, 0.0f, 0.0f), at(0));
    play(m, 0, 500);
    m.report(running(500, 3.5f, 0.0f), at(500));
    play(m, 500, 1000);
    m.report(running(1000, 7.0f, 0.0f, kHalfPi), at(1000));  // turns to +y
    play(m, 1000, 1500);
    m.report(running(1500, 7.0f, 3.5f, kHalfPi), at(1500));
    play(m, 1500, 2000);

    REQUIRE(m.playbackRate() == Catch::Approx(1.0f).margin(0.05f));
    const double since = (m.playbackTime() - 1000.0) / 1000.0;
    REQUIRE(m.position().x == Catch::Approx(7.0f).margin(0.02f));
    REQUIRE(m.position().y == Catch::Approx(7.0 * since).margin(0.02f));
}

TEST_CASE("Reported movement replays a late burst faster, every turn in its place",
          "[entity][movement]") {
    ReportedMovement m;
    m.report(running(0, 0.0f, 0.0f), at(0));
    // Two seconds with nothing: it runs on as it last said, then waits.
    play(m, 0, 2000);
    REQUIRE(m.position().x == Catch::Approx(7.0f * 0.75f).margin(0.02f));

    // The two seconds arrive at once. The turn at 1000 ms is still ahead of
    // playback, so it is taken where it happened.
    m.report(running(500, 3.5f, 0.0f), at(2000));
    m.report(running(1000, 7.0f, 0.0f, kHalfPi), at(2000));
    m.report(running(1500, 7.0f, 3.5f, kHalfPi), at(2000));
    m.report(running(2000, 7.0f, 7.0f, kHalfPi), at(2000));
    play(m, 2000, 2010);
    REQUIRE(m.playbackRate() > 2.0f);

    play(m, 2010, 4000);
    REQUIRE(m.position().x == Catch::Approx(7.0f).margin(0.02f));
    // Caught up to the newest report and on past it as far as playback goes.
    REQUIRE(m.position().y == Catch::Approx(7.0f + 7.0f * 0.75f).margin(0.05f));
}

namespace {

// A wall across the x axis at x = 5, for a mover a yard thick.
struct WallAtFive final : MovementCollision {
    glm::vec3 slide(const glm::vec3& from, const glm::vec3& to) const override {
        glm::vec3 out = to;
        if (from.x <= 4.5f && to.x > 4.5f) out.x = 4.5f;
        return out;
    }
};

}  // namespace

TEST_CASE("Reported movement stays at a wall a runner holds forward into", "[entity][movement]") {
    ReportedMovement m;
    const WallAtFive wall;
    m.report(running(0, 0.0f, 0.0f), at(0));
    for (int t = 10; t <= 1000; t += 10) m.advance(0.01f, at(t), MovementSpeeds{}, &wall);
    REQUIRE(m.isTravelling());
    REQUIRE(m.position().x == Catch::Approx(4.5f));
}

TEST_CASE("Reported movement stops where a stop report says", "[entity][movement]") {
    ReportedMovement m;
    MovementReport stop = running(0, 3.0f, 0.0f);
    stop.flags = 0;
    m.report(stop, at(0));
    play(m, 0, 1000);
    REQUIRE_FALSE(m.isMoving());
    REQUIRE(m.position().x == Catch::Approx(3.0f));
}

TEST_CASE("Reported movement flies a jump on its arc", "[entity][movement]") {
    ReportedMovement m;
    MovementReport jump = running(0, 0.0f, 0.0f);
    jump.flags |= moveBit(MovementFlags::FALLING);
    jump.jumpUpSpeed = 7.955547f;
    jump.jumpCos = 1.0f;
    jump.jumpSin = 0.0f;
    jump.jumpXYSpeed = 7.0f;
    m.report(jump, at(0));
    REQUIRE(m.isAirborne());
    REQUIRE(m.isJump());

    play(m, 0, 300);
    const float g = 19.29110527f;
    const float t = static_cast<float>(m.playbackTime() / 1000.0);
    REQUIRE(t > 0.2f);
    REQUIRE(m.position().z == Catch::Approx(10.0f + 7.955547f * t - 0.5f * g * t * t).margin(0.01f));
    REQUIRE(m.position().x == Catch::Approx(7.0f * t).margin(0.01f));
}

TEST_CASE("Reported movement drops a report older than one it took", "[entity][movement]") {
    ReportedMovement m;
    m.report(running(1000, 10.0f, 0.0f), at(1000));
    REQUIRE_FALSE(m.report(running(500, 3.0f, 0.0f), at(1010)));
    REQUIRE(m.position().x == Catch::Approx(10.0f));
}

TEST_CASE("Reported movement follows the mover's clock across its wrap", "[entity][movement]") {
    ReportedMovement m;
    m.report(running(0xFFFFFF00u, 0.0f, 0.0f), at(0));
    REQUIRE(m.report(running(0x00000000u, 1.8f, 0.0f), at(256)));  // 256 ms later
    REQUIRE(m.lastReportLateness() == Catch::Approx(0.0f));
}

TEST_CASE("A unit that reports its movement leaves it for a server path", "[entity][movement]") {
    Unit u;
    u.applyMovementReport(running(0, 0.0f, 0.0f), at(0));
    REQUIRE(u.getReportedMovement() != nullptr);
    REQUIRE(u.isEntityMoving());

    u.startMoveTo(20.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    REQUIRE(u.getReportedMovement() == nullptr);
    u.updateMovement(2.0f);
    REQUIRE(u.getX() == Catch::Approx(20.0f));
}

TEST_CASE("Entity field set/get/has", "[entity]") {
    Entity e;
    REQUIRE_FALSE(e.hasField(10));

    e.setField(10, 0xCAFE);
    REQUIRE(e.hasField(10));
    REQUIRE(e.getField(10) == 0xCAFE);

    // Overwrite
    e.setField(10, 0xBEEF);
    REQUIRE(e.getField(10) == 0xBEEF);

    // Non-existent returns 0
    REQUIRE(e.getField(999) == 0);
}

TEST_CASE("Unit construction and type", "[entity]") {
    Unit u;
    REQUIRE(u.getType() == ObjectType::UNIT);

    Unit u2(0x123);
    REQUIRE(u2.getGuid() == 0x123);
    REQUIRE(u2.getType() == ObjectType::UNIT);
}

TEST_CASE("Unit name", "[entity]") {
    Unit u;
    REQUIRE(u.getName().empty());
    u.setName("Hogger");
    REQUIRE(u.getName() == "Hogger");
}

TEST_CASE("Unit health", "[entity]") {
    Unit u;
    REQUIRE(u.getHealth() == 0);
    REQUIRE(u.getMaxHealth() == 0);

    u.setHealth(500);
    u.setMaxHealth(1000);
    REQUIRE(u.getHealth() == 500);
    REQUIRE(u.getMaxHealth() == 1000);
}

TEST_CASE("Unit power by type", "[entity]") {
    Unit u;
    u.setPowerType(0); // mana
    u.setPower(200);
    u.setMaxPower(500);

    REQUIRE(u.getPower() == 200);
    REQUIRE(u.getMaxPower() == 500);
    REQUIRE(u.getPowerByType(0) == 200);
    REQUIRE(u.getMaxPowerByType(0) == 500);

    // Set rage (type 1)
    u.setPowerByType(1, 50);
    u.setMaxPowerByType(1, 100);
    REQUIRE(u.getPowerByType(1) == 50);
    REQUIRE(u.getMaxPowerByType(1) == 100);

    // Out of bounds clamps
    REQUIRE(u.getPowerByType(7) == 0);
    REQUIRE(u.getMaxPowerByType(7) == 0);
}

TEST_CASE("Unit level, entry, displayId", "[entity]") {
    Unit u;
    REQUIRE(u.getLevel() == 1); // default
    u.setLevel(80);
    REQUIRE(u.getLevel() == 80);

    u.setEntry(1234);
    REQUIRE(u.getEntry() == 1234);

    u.setDisplayId(5678);
    REQUIRE(u.getDisplayId() == 5678);
}

TEST_CASE("Unit flags", "[entity]") {
    Unit u;
    u.setUnitFlags(0x01);
    REQUIRE(u.getUnitFlags() == 0x01);

    REQUIRE_FALSE(u.hasCreepVisibility());
    u.setVisibilityFlags(UNIT_VIS_FLAG_CREEP);
    REQUIRE(u.getVisibilityFlags() == UNIT_VIS_FLAG_CREEP);
    REQUIRE(u.hasCreepVisibility());
    u.clearCreepVisibility();
    REQUIRE_FALSE(u.hasCreepVisibility());
    REQUIRE((u.getVisibilityFlags() & UNIT_VIS_FLAG_CREEP) == 0);

    u.setDynamicFlags(0x02);
    REQUIRE(u.getDynamicFlags() == 0x02);

    u.setNpcFlags(0x04);
    REQUIRE(u.getNpcFlags() == 0x04);
    REQUIRE(u.isInteractable());

    u.setNpcFlags(0);
    REQUIRE_FALSE(u.isInteractable());
}

TEST_CASE("Corpse state uses the real dynamic dead bit", "[entity][corpse]") {
    REQUIRE(isUnitCorpseState(0, 100, 0));
    REQUIRE(isUnitCorpseState(0, 0, UNIT_DYNFLAG_DEAD));
    REQUIRE(isUnitCorpseState(0, 0, UNIT_DYNFLAG_LOOTABLE));

    REQUIRE_FALSE(isUnitCorpseState(100, 100, 0));
    // Regression: 0x08 is tapped-by-player, not dead.
    REQUIRE_FALSE(isUnitCorpseState(100, 100, UNIT_DYNFLAG_TAPPED_BY_PLAYER));
    REQUIRE(UNIT_DYNFLAG_DEAD == 0x00000020);
}

TEST_CASE("Unit faction and hostility", "[entity]") {
    Unit u;
    u.setFactionTemplate(14); // Undercity
    REQUIRE(u.getFactionTemplate() == 14);

    REQUIRE_FALSE(u.isHostile());
    u.setHostile(true);
    REQUIRE(u.isHostile());
}

TEST_CASE("Unit mount display ID", "[entity]") {
    Unit u;
    REQUIRE(u.getMountDisplayId() == 0);
    u.setMountDisplayId(14374);
    REQUIRE(u.getMountDisplayId() == 14374);
}

TEST_CASE("Player inherits Unit", "[entity]") {
    Player p(0xABC);
    REQUIRE(p.getType() == ObjectType::PLAYER);
    REQUIRE(p.getGuid() == 0xABC);

    // Player inherits Unit name - regression test for the shadowed-field fix
    p.setName("Arthas");
    REQUIRE(p.getName() == "Arthas");

    p.setLevel(80);
    REQUIRE(p.getLevel() == 80);
}

TEST_CASE("GameObject construction", "[entity]") {
    GameObject go(0x999);
    REQUIRE(go.getType() == ObjectType::GAMEOBJECT);
    REQUIRE(go.getGuid() == 0x999);

    go.setName("Mailbox");
    REQUIRE(go.getName() == "Mailbox");

    go.setEntry(42);
    REQUIRE(go.getEntry() == 42);

    go.setDisplayId(100);
    REQUIRE(go.getDisplayId() == 100);
}

TEST_CASE("EntityManager add/get/has/remove", "[entity]") {
    EntityManager mgr;
    REQUIRE(mgr.getEntityCount() == 0);

    auto unit = std::make_shared<Unit>(1);
    unit->setName("TestUnit");
    mgr.addEntity(1, unit);

    REQUIRE(mgr.getEntityCount() == 1);
    REQUIRE(mgr.hasEntity(1));
    REQUIRE_FALSE(mgr.hasEntity(2));

    auto retrieved = mgr.getEntity(1);
    REQUIRE(retrieved != nullptr);
    REQUIRE(retrieved->getGuid() == 1);

    mgr.removeEntity(1);
    REQUIRE_FALSE(mgr.hasEntity(1));
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager clear", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, std::make_shared<Entity>(1));
    mgr.addEntity(2, std::make_shared<Entity>(2));
    REQUIRE(mgr.getEntityCount() == 2);

    mgr.clear();
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager null entity rejected", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, nullptr);
    // Null should be rejected (logged warning, not stored)
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager getEntities returns all", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(10, std::make_shared<Unit>(10));
    mgr.addEntity(20, std::make_shared<Player>(20));
    mgr.addEntity(30, std::make_shared<GameObject>(30));

    const auto& all = mgr.getEntities();
    REQUIRE(all.size() == 3);
    REQUIRE(all.count(10) == 1);
    REQUIRE(all.count(20) == 1);
    REQUIRE(all.count(30) == 1);
}

TEST_CASE("EntityManager spatial query filters nearby entities", "[entity][spatial]") {
    EntityManager mgr;
    auto origin = std::make_shared<Unit>(1);
    origin->setPosition(-10.0f, -10.0f, 0.0f, 0.0f);
    auto nearby = std::make_shared<Unit>(2);
    nearby->setPosition(20.0f, 15.0f, 0.0f, 0.0f);
    auto distant = std::make_shared<Unit>(3);
    distant->setPosition(500.0f, 500.0f, 0.0f, 0.0f);
    mgr.addEntity(1, origin);
    mgr.addEntity(2, nearby);
    mgr.addEntity(3, distant);

    const auto result = mgr.getEntitiesNear(0.0f, 0.0f, 50.0f);
    REQUIRE(result.size() == 2);
    REQUIRE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 1; }));
    REQUIRE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 2; }));
    REQUIRE_FALSE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 3; }));
}
