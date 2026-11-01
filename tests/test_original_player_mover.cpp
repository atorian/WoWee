// The player moved by the original client's rules, switched by the
// originalMovement setting: the real game in GameHarness, the map as the
// original collects it, and what the server is sent.
#include <catch_amalgamated.hpp>

#include "collision/movement_world.hpp"
#include "collision/terrain_heights.hpp"
#include "core/cvar_store.hpp"
#include "core/original_player_mover.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"
#include "game_harness.hpp"
#include "test_game_tables.hpp"
#include "pipeline/adt_loader.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using wowee::game::Opcode;
using wowee::game::UF;
using wowee::game::fieldIndex;
using wowee::test::Body;
using wowee::test::GameHarness;

namespace {

constexpr uint32_t kHumanMaleDisplay = 49;

/// WotLK's column layouts, as the client reads them.
std::string wotlkLayouts() {
    std::ifstream file(WOWEE_SOURCE_DIR "/Data/expansions/wotlk/dbc_layouts.json");
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/// Tile (32, 32), flat at height 0: it spans x and y from -533 to 0.
std::shared_ptr<const wowee::collision::TerrainTileHeights> flatTile() {
    auto terrain = std::make_unique<wowee::pipeline::ADTTerrain>();
    for (auto& chunk : terrain->chunks) {
        chunk.loaded = true;
        chunk.heightMap.loaded = true;
        chunk.heightMap.heights.fill(0.0f);
    }
    return wowee::collision::TerrainTileHeights::build(*terrain, 32, 32);
}

/// A logged-in human standing at (-10, -10, 0) on that tile, moved by the
/// original's rules when `on`.
struct World {
    wowee::test::GivenLayouts layouts{wotlkLayouts()};
    wowee::core::CVarStore settings;
    wowee::core::OriginalPlayerMover mover{settings};
    GameHarness h{mover};
    wowee::collision::MovementWorld map;
    wowee::test::PlayerSpec spec;

    explicit World(bool on) {
        settings.set(wowee::core::OriginalPlayerMover::kSetting, on ? "1" : "0");
        h.dbc("CreatureDisplayInfo.dbc", 16, {{{0, kHumanMaleDisplay}, {1, 49u}, {4, 1.0f}}});
        h.dbc("CreatureModelData.dbc", 28, {{{0, 49u}, {14, 0.611f}, {15, 2.031f}}});
        std::bitset<64 * 64> tiles;
        tiles.set(32 * 64 + 32);
        map.beginMap(0, tiles);
        map.addTile(flatTile());
        mover.connect(map, h.game(), h.gameData());
        spec.x = -10.0f;
        spec.y = -10.0f;
        spec.z = 0.0f;
        h.enterWorld(spec);
        h.receive(Opcode::SMSG_UPDATE_OBJECT,
                  GameHarness::createObjectBody(spec.guid, wowee::game::ObjectType::PLAYER,
                                                {{fieldIndex(UF::UNIT_FIELD_DISPLAYID), kHumanMaleDisplay},
                                                 {fieldIndex(UF::UNIT_FIELD_HEALTH), 100}},
                                                spec.x, spec.y, spec.z));
        h.pump();
        h.clearRecords();
    }

    /// A frame of the camera's: ask, then move with these controls.
    wowee::rendering::CharacterMotion frame(const glm::vec3& at, const wowee::rendering::CharacterIntent& intent) {
        REQUIRE(mover.movesCharacter(at, 0.0f));
        return mover.move(intent);
    }

    void forceRunSpeed(uint32_t counter, float speed) {
        h.receive(Opcode::SMSG_FORCE_RUN_SPEED_CHANGE,
                  Body().packedGuid(spec.guid).u32(counter).u8(0).f32(speed));
        h.pump();
    }
};

wowee::game::MovementInfo statusOf(wowee::network::Packet& packet) {
    REQUIRE(packet.readPackedGuid() != 0);
    wowee::game::MovementInfo status;
    REQUIRE(wowee::game::MovementPacket::readMovementPayload(packet, status));
    return status;
}

void wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST_CASE("with the setting on, the player runs on the map's terrain and the server is told",
          "[movement][original]") {
    World w(true);
    wowee::rendering::CharacterIntent run;
    run.forward = true;

    // The input is queued at this frame's time and executed as time passes
    // it, as the original's ExecuteMovement does: the start goes out from
    // where the player stood.
    auto motion = w.frame({-10, -10, 0}, run);
    for (int i = 0; i < 4; ++i) {
        wait(30);
        motion = w.frame(motion.position, run);
    }
    auto* start = w.h.lastSent(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(start != nullptr);
    const auto started = statusOf(*start);
    CHECK(started.x == Catch::Approx(-10.0f));
    CHECK(started.y == Catch::Approx(-10.0f));
    // Facing 0 is the server's +x; nothing but the ground was in the way.
    CHECK(motion.position.x > -10.0f + 0.2f);
    CHECK(motion.position.y == Catch::Approx(-10.0f).margin(1e-3));
    CHECK(motion.position.z == Catch::Approx(0.0f).margin(1e-3));
    CHECK(motion.grounded);

    motion = w.frame(motion.position, {});
    wait(30);
    motion = w.frame(motion.position, {});
    auto* stop = w.h.lastSent(Opcode::MSG_MOVE_STOP);
    REQUIRE(stop != nullptr);
    const auto stopped = statusOf(*stop);
    CHECK(stopped.x == Catch::Approx(motion.position.x));
    CHECK((stopped.flags & 0x1u) == 0);
}

TEST_CASE("a forced speed is acknowledged when the original applies it, not on arrival",
          "[movement][original]") {
    World w(true);
    auto motion = w.frame({-10, -10, 0}, {});
    w.forceRunSpeed(0x55, 14.0f);
    CHECK(w.h.lastSent(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK) == nullptr);

    wait(30);
    motion = w.frame(motion.position, {});
    auto* ack = w.h.lastSent(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK);
    REQUIRE(ack != nullptr);
    CHECK(ack->readPackedGuid() == w.spec.guid);
    CHECK(ack->readUInt32() == 0x55);
    wowee::game::MovementInfo status;
    REQUIRE(wowee::game::MovementPacket::readMovementPayload(*ack, status));
    REQUIRE(ack->hasRemaining(4));
    CHECK(ack->readFloat() == 14.0f);
}

TEST_CASE("with the setting off, the camera keeps the player and the game acknowledges at once",
          "[movement][original]") {
    World w(false);
    CHECK_FALSE(w.mover.movesCharacter({-10, -10, 0}, 0.0f));
    CHECK_FALSE(w.mover.reportsMovement());
    w.forceRunSpeed(0x56, 9.0f);
    CHECK(w.h.lastSent(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK) != nullptr);
}

TEST_CASE("a player teleported by the server starts again where it was put", "[movement][original]") {
    World w(true);
    wowee::rendering::CharacterIntent run;
    run.forward = true;
    (void)w.frame({-10, -10, 0}, run);
    wait(30);
    // The camera says the player is elsewhere now: a teleport.
    const auto motion = w.frame({-100, -100, 0}, run);
    CHECK(motion.position.x == Catch::Approx(-100.0f).margin(0.5f));
    CHECK(motion.position.y == Catch::Approx(-100.0f).margin(1e-3));
}
