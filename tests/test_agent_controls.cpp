// An agent drives the character only while the player has handed control over.
//
// The requirement: nothing moves without /ai on; any input of the player's own
// takes control back and stops whatever was under way; a move holds its key
// for the time asked and then lets go; a jump is a single press; a turn moves
// facing and view together, a look the view alone.
#include <catch_amalgamated.hpp>

#include "ui/agent_controls.hpp"

using wowee::ui::AgentControls;
using Move = AgentControls::Move;

namespace {
bool held(const AgentControls::Frame& f, Move m) { return f.held[static_cast<size_t>(m)]; }
}

TEST_CASE("without control every command is refused and nothing is held", "[agent]") {
    AgentControls agent;
    CHECK_FALSE(agent.move(Move::Forward, 1.0f));
    CHECK_FALSE(agent.turn(90.0f));
    CHECK_FALSE(agent.look(10.0f, 5.0f));
    CHECK_FALSE(agent.zoom(1.0f));
    CHECK_FALSE(agent.jump());
    const auto f = agent.advance(0.1f);
    CHECK_FALSE(held(f, Move::Forward));
    CHECK_FALSE(f.jump);
    CHECK(f.turnDegrees == 0.0f);
}

TEST_CASE("a move holds its key for the time asked, then lets go", "[agent]") {
    AgentControls agent;
    agent.grant();
    REQUIRE(agent.move(Move::Forward, 0.5f));
    CHECK(held(agent.advance(0.2f), Move::Forward));
    CHECK(held(agent.advance(0.2f), Move::Forward));
    CHECK_FALSE(held(agent.advance(0.2f), Move::Forward));   // 0.6 s > 0.5 s
}

TEST_CASE("a move shorter than a frame is still held for one", "[agent]") {
    AgentControls agent;
    agent.grant();
    REQUIRE(agent.move(Move::Backward, 0.005f));
    CHECK(held(agent.advance(0.016f), Move::Backward));
    CHECK_FALSE(held(agent.advance(0.016f), Move::Backward));
}

TEST_CASE("moves in different directions run together", "[agent]") {
    AgentControls agent;
    agent.grant();
    agent.move(Move::Forward, 1.0f);
    agent.move(Move::StrafeLeft, 0.3f);
    auto f = agent.advance(0.2f);
    CHECK(held(f, Move::Forward));
    CHECK(held(f, Move::StrafeLeft));
    f = agent.advance(0.2f);
    CHECK(held(f, Move::Forward));
    CHECK_FALSE(held(f, Move::StrafeLeft));
}

TEST_CASE("a jump is one press", "[agent]") {
    AgentControls agent;
    agent.grant();
    REQUIRE(agent.jump());
    CHECK(agent.advance(0.016f).jump);
    CHECK_FALSE(agent.advance(0.016f).jump);
}

TEST_CASE("a turn steers the character, a look moves only the view", "[agent]") {
    AgentControls agent;
    agent.grant();

    SECTION("turn") {
        agent.turn(90.0f);
        const auto f = agent.advance(0.016f);
        CHECK(f.turnDegrees == 90.0f);
        CHECK(agent.isSteering());
        CHECK(agent.advance(0.016f).turnDegrees == 0.0f);
        CHECK_FALSE(agent.isSteering());
    }
    SECTION("look") {
        agent.look(-30.0f, 10.0f);
        const auto f = agent.advance(0.016f);
        CHECK(f.lookYawDegrees == -30.0f);
        CHECK(f.lookPitchDegrees == 10.0f);
        CHECK(f.turnDegrees == 0.0f);
        CHECK_FALSE(agent.isSteering());
    }
    SECTION("two turns before a frame add up") {
        agent.turn(30.0f);
        agent.turn(15.0f);
        CHECK(agent.advance(0.016f).turnDegrees == 45.0f);
    }
}

TEST_CASE("the player's own input takes control back and stops everything", "[agent]") {
    AgentControls agent;
    agent.grant();
    agent.move(Move::Forward, 5.0f);
    agent.turn(90.0f);
    agent.onPlayerInput();
    CHECK_FALSE(agent.hasControl());
    const auto f = agent.advance(0.016f);
    CHECK_FALSE(held(f, Move::Forward));
    CHECK(f.turnDegrees == 0.0f);
    CHECK_FALSE(agent.move(Move::Forward, 1.0f));
}

TEST_CASE("keys are set while the agent drives, let go once, then left alone", "[agent]") {
    AgentControls agent;
    CHECK_FALSE(agent.advance(0.016f).setKeys);   // the player's keys are not touched
    agent.grant();
    agent.move(Move::Forward, 5.0f);
    CHECK(agent.advance(0.016f).setKeys);
    agent.release();
    const auto letGo = agent.advance(0.016f);
    CHECK(letGo.setKeys);
    CHECK_FALSE(held(letGo, Move::Forward));
    CHECK_FALSE(agent.advance(0.016f).setKeys);
}

TEST_CASE("stop lets go of every key at once", "[agent]") {
    AgentControls agent;
    agent.grant();
    agent.move(Move::Backward, 5.0f);
    agent.move(Move::StrafeRight, 5.0f);
    agent.stop();
    const auto f = agent.advance(0.016f);
    CHECK_FALSE(held(f, Move::Backward));
    CHECK_FALSE(held(f, Move::StrafeRight));
    CHECK(agent.hasControl());
}
