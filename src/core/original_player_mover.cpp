#include "core/original_player_mover.hpp"

#include "collision/movement_dimensions.hpp"
#include "collision/movement_execution.hpp"
#include "collision/movement_world.hpp"
#include "core/coordinates.hpp"
#include "core/cvar_store.hpp"
#include "core/logger.hpp"
#include "core/movement_reporting.hpp"
#include "core/movement_status.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/movement_flags.hpp"
#include "game/movement_handler.hpp"
#include "game/opcode_table.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/game_tables.hpp"
#include "pipeline/required_table.hpp"

#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>

namespace wowee::core {

namespace {

using game::MovementFlags;
using game::Opcode;

constexpr uint32_t flag(MovementFlags f) { return static_cast<uint32_t>(f); }

/// The states a player is put in that outlast a step: carried into the
/// original's state when it takes the player over.
constexpr uint32_t kCarriedFlags =
    flag(MovementFlags::WALKING) | flag(MovementFlags::LEVITATING) | flag(MovementFlags::ROOT) |
    flag(MovementFlags::SWIMMING) | flag(MovementFlags::CAN_FLY) | flag(MovementFlags::FLYING) |
    flag(MovementFlags::WATER_WALK) | flag(MovementFlags::FEATHER_FALL) | flag(MovementFlags::HOVER);

/// A model's collision box, CreatureModelData's, and the display's scale.
struct ModelBox {
    float width = 0.0f;
    float height = 0.0f;
    float displayScale = 1.0f;
};

std::optional<ModelBox> modelBox(const pipeline::GameTables& tables, uint32_t displayId) {
    constexpr const char* kNeededFor = "the player's collision size, for the original movement";
    const pipeline::RequiredTable displays(tables, "CreatureDisplayInfo.dbc", kNeededFor);
    const int32_t display = displays->findRecordById(displayId);
    if (display < 0) return std::nullopt;
    const pipeline::RequiredTable models(tables, "CreatureModelData.dbc", kNeededFor);
    const int32_t model = models->findRecordById(
        displays->getUInt32(static_cast<uint32_t>(display), displays.column("ModelID")));
    if (model < 0) return std::nullopt;
    ModelBox box;
    box.width = models->getFloat(static_cast<uint32_t>(model), models.column("CollisionWidth"));
    box.height = models->getFloat(static_cast<uint32_t>(model), models.column("CollisionHeight"));
    box.displayScale = displays->getFloat(static_cast<uint32_t>(display), displays.column("CreatureModelScale"));
    if (box.displayScale <= 0.0f) box.displayScale = 1.0f;
    return box;
}

float objectScale(const game::Entity& entity) {
    const uint16_t index = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    if (index == 0xFFFF) return 1.0f;
    const uint32_t raw = entity.getField(index);
    float scale = 1.0f;
    if (raw != 0) std::memcpy(&scale, &raw, sizeof(scale));
    return scale > 0.01f && scale < 100.0f ? scale : 1.0f;
}

std::optional<collision::MovementRate> rateFor(Opcode ack) {
    using collision::MovementRate;
    switch (ack) {
    case Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK: return MovementRate::Walk;
    case Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK: return MovementRate::Run;
    case Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK: return MovementRate::RunBack;
    case Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK: return MovementRate::Swim;
    case Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK: return MovementRate::SwimBack;
    case Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK: return MovementRate::Flight;
    case Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK: return MovementRate::FlightBack;
    case Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK: return MovementRate::Turn;
    case Opcode::CMSG_FORCE_PITCH_RATE_CHANGE_ACK: return MovementRate::Pitch;
    default: return std::nullopt;
    }
}

/// A mode and whether the command turns it on.
std::optional<std::pair<collision::MovementMode, bool>> modeFor(const game::ForcedMovement& command) {
    using collision::MovementMode;
    switch (command.acknowledgement) {
    case Opcode::CMSG_FORCE_MOVE_ROOT_ACK: return std::pair{MovementMode::Root, true};
    case Opcode::CMSG_FORCE_MOVE_UNROOT_ACK: return std::pair{MovementMode::Root, false};
    case Opcode::CMSG_MOVE_HOVER_ACK: return std::pair{MovementMode::Hover, command.enabled};
    case Opcode::CMSG_MOVE_FEATHER_FALL_ACK: return std::pair{MovementMode::FeatherFall, command.enabled};
    case Opcode::CMSG_MOVE_WATER_WALK_ACK: return std::pair{MovementMode::WaterWalk, command.enabled};
    case Opcode::CMSG_MOVE_SET_CAN_FLY_ACK: return std::pair{MovementMode::CanFly, command.enabled};
    case Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK:
        return std::pair{MovementMode::SwimFlyTransition, command.enabled};
    // The gravity mode is "gravity on": disabling it turns it off.
    case Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK: return std::pair{MovementMode::Gravity, false};
    case Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK: return std::pair{MovementMode::Gravity, true};
    default: return std::nullopt;
    }
}

uint64_t tileKey(const glm::vec3& position) {
    const auto [x, y] = coords::worldToTile(position.x, position.y);
    return static_cast<uint64_t>(y) * 64 + static_cast<uint64_t>(x);
}

}  // namespace

struct OriginalPlayerMover::State final : collision::MovementExecutionSink,
                                          MovementReportConnection,
                                          MovementLocalEffects {
    explicit State(const CVarStore& s) : settings(s) {}

    const CVarStore& settings;
    collision::MovementWorld* world = nullptr;
    game::GameHandler* game = nullptr;
    const pipeline::GameTables* tables = nullptr;

    bool active = false;
    /// Why the player is not this mover's, as last said; empty while it is.
    std::string reason = "off";
    /// The tile whose collision failed: the player stays the camera's there.
    std::optional<uint64_t> failedTile;
    bool waitingForMap = false;

    collision::SurfaceMovementState movement;
    collision::SurfaceMovementShape shape{0.3f, 2.0f, 1.0f, true};
    uint32_t displayId = 0;
    float scale = 0.0f;
    collision::MovementCommandQueue commands;
    collision::MovementRequestCache cache;
    collision::MovementClock clock;
    MovementReportingState reporting;
    uint32_t lastTime = 0;
    uint32_t eventTime = 0;
    rendering::CharacterIntent lastIntent;
    glm::vec3 lastPosition{0.0f};
    std::unordered_set<uint16_t> unknownOpcodes;
    std::unordered_set<int> unownedCommands;

    // ---- What it decides -------------------------------------------------

    /// Empty when it can move the player now; otherwise why not.
    std::string unsupported(const glm::vec3& position) const {
        if (settings.get(kSetting, "0") != "1") return "off";
        if (!world || !game) return "not connected to a world";
        if (!game->isInWorld()) return "not in a world";
        if (!game::isActiveExpansion("wotlk")) {
            return "the original movement is build 12340's, and this expansion is not WotLK";
        }
        const auto& handler = game->movement();
        if (handler.isOnTransport()) return "the player is on a transport, which it does not attach to yet";
        if (handler.isOnTaxiFlight() || handler.isTaxiMountActive()) return "the player is on a taxi";
        if (!world->hasMap()) return "the map's collision has not begun";
        if (failedTile && *failedTile == tileKey(position)) return "the collision of the area failed to load";
        return {};
    }

    void say(const std::string& why) {
        if (why == reason) return;
        if (why.empty()) {
            LOG_WARNING("Original movement: on - the player moves by build 12340's rules");
        } else if (why == "off" || why == "not in a world" || why == "not connected to a world") {
            if (reason.empty()) LOG_WARNING("Original movement: off (", why, ") - the camera moves the player");
        } else {
            LOG_WARNING("Original movement: not moving the player - ", why, "; the camera moves the player");
        }
        reason = why;
    }

    // ---- Taking the player over ------------------------------------------

    bool fitShape() {
        const game::Entity* player = game->getEntityManager().getEntity(game->getPlayerGuid()).get();
        const auto* unit = dynamic_cast<const game::Unit*>(player);
        if (!unit) return false;
        const float unitScale = objectScale(*unit);
        if (unit->getDisplayId() == displayId && unitScale == scale) return true;
        const auto box = modelBox(*tables, unit->getDisplayId());
        if (!box) {
            LOG_ERROR("Original movement: display ", unit->getDisplayId(),
                      " has no model collision box - the player cannot be moved by it");
            return false;
        }
        // SetCollisionBox with the display's model box. Its arguments from the
        // display and the object scale are not verified against the original.
        collision::setMovementModelDimensions(shape, box->width, box->height,
                                              unitScale * box->displayScale, box->displayScale,
                                              displayId == 0);
        displayId = unit->getDisplayId();
        scale = unitScale;
        return true;
    }

    void seat(const glm::vec3& position, float facingDeg) {
        const auto& handler = game->movement();
        movement = collision::SurfaceMovementState{};
        movement.position = movement.anchorPosition = position;
        movement.facing = movement.anchorFacing = coords::normalizeAngleRad(glm::radians(facingDeg));
        movement.groundNormal = glm::vec3(0.0f, 0.0f, 1.0f);
        movement.flags = game->getMovementInfo().flags & kCarriedFlags;
        movement.walkSpeed = handler.getServerWalkSpeed();
        movement.runSpeed = handler.getServerRunSpeed();
        movement.runBackSpeed = handler.getServerRunBackSpeed();
        movement.swimSpeed = handler.getServerSwimSpeed();
        movement.swimBackSpeed = handler.getServerSwimBackSpeed();
        movement.flightSpeed = handler.getServerFlightSpeed();
        movement.flightBackSpeed = handler.getServerFlightBackSpeed();
        movement.turnRate = handler.getServerTurnRate();
        movement.pitchRate = coords::PI;
        commands = collision::MovementCommandQueue{};
        cache = collision::MovementRequestCache{};
        clock = collision::MovementClock{};
        reporting = MovementReportingState{};
        reporting.facing = movement.facing;
        lastTime = handler.movementClockMs();
        lastIntent = rendering::CharacterIntent{};
        lastPosition = position;
    }

    // ---- Execution's outputs ---------------------------------------------

    std::optional<Opcode> logical(uint32_t wire) {
        const auto* table = game::getActiveOpcodeTable();
        const auto found = table ? table->fromWire(static_cast<uint16_t>(wire)) : std::nullopt;
        if (!found && unknownOpcodes.insert(static_cast<uint16_t>(wire)).second) {
            LOG_ERROR("Original movement: message 0x", std::hex, wire, std::dec,
                      " is not in the opcode table - it is not sent");
        }
        return found;
    }

    bool deliver(uint32_t time, uint32_t wire, bool needAck, uint32_t counter = 0, float value = 0) {
        const auto opcode = logical(wire);
        if (!opcode) return false;
        eventTime = time;
        return processLocalMovementEvent12340(movement, reporting, clock, *this, *this, game->getPlayerGuid(),
                                              true, true, 0, time, *opcode, needAck, counter, value);
    }

    void collisionHeightChanged(uint32_t time, float height, uint32_t counter, bool needAck) override {
        (void)deliver(time, game::wireOpcode(Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK), needAck, counter, height);
    }
    void movementEffect(const collision::MovementCommandEffect&) override {}
    bool forcedMovementChanged(uint32_t time, uint32_t opcode, float value, uint32_t counter,
                               bool needAck) override {
        return deliver(time, opcode, needAck, counter, value);
    }
    bool inputChanged(uint32_t time, uint32_t opcode, bool needAck) override {
        return deliver(time, opcode, needAck);
    }
    void angularInputStopped(bool) override {}
    void resetHeartbeat(uint32_t time) override { clock.heartbeatTimeMs = time + 500u; }
    bool heartbeat(uint32_t time) override {
        eventTime = time;
        return reportUnattachedMovement12340(movement, reporting, clock, *this, game->getPlayerGuid(), true, time,
                                             Opcode::MSG_MOVE_HEARTBEAT);
    }
    void frameFinished(uint32_t) override {}

    void sendMovement(const network::Packet& packet) override {
        const auto opcode = logical(packet.getOpcode());
        if (!opcode) return;
        game->movement().sendReportedMovement(*opcode, makeUnattachedMovementStatus12340(movement, eventTime),
                                              packet);
    }
    void refreshMovementEffects() override {}
    void standUp() override {}
    void updateInput(uint32_t) override {}
    void movementBecameIdle() override {}
    void notifyMovement(Opcode) override {}

    // ---- The player's input ----------------------------------------------

    void queue(uint32_t time, collision::MovementInput input) { commands.enqueueInput(time, input, true); }

    void queueInput(const rendering::CharacterIntent& intent, uint32_t time) {
        using collision::MovementInput;
        const auto axis = [&](bool positive, bool negative, bool wasPositive, bool wasNegative,
                              MovementInput start, MovementInput startNegative, MovementInput stop) {
            const int now = positive == negative ? 0 : (positive ? 1 : -1);
            const int before = wasPositive == wasNegative ? 0 : (wasPositive ? 1 : -1);
            if (now == before) return;
            queue(time, now == 0 ? stop : (now > 0 ? start : startNegative));
        };
        axis(intent.forward, intent.backward, lastIntent.forward, lastIntent.backward,
             MovementInput::Forward, MovementInput::Backward, MovementInput::Stop);
        axis(intent.strafeLeft, intent.strafeRight, lastIntent.strafeLeft, lastIntent.strafeRight,
             MovementInput::StrafeLeft, MovementInput::StrafeRight, MovementInput::StopStrafe);
        axis(intent.turnLeft, intent.turnRight, lastIntent.turnLeft, lastIntent.turnRight,
             MovementInput::TurnLeft, MovementInput::TurnRight, MovementInput::StopTurn);
        axis(intent.ascend, intent.descend, lastIntent.ascend, lastIntent.descend,
             MovementInput::Ascend, MovementInput::Descend, MovementInput::StopAscend);
        if (intent.jump) queue(time, MovementInput::Jump);
        lastIntent = intent;
    }

    /// The view steering the character: its facing, and its pitch where the
    /// state permits one. Re-anchored where it stands, and told the server
    /// the way the original's SetFacing and SetPitch are - not verified.
    void steer(const rendering::CharacterIntent& intent, uint32_t time) {
        if (!intent.steeredFacingDeg) return;
        const float facing = coords::normalizeAngleRad(glm::radians(*intent.steeredFacingDeg));
        if (facing != movement.facing) {
            movement.facing = movement.anchorFacing = facing;
            movement.anchorPosition = movement.position;
            movement.anchorElapsedMs = 0;
            (void)deliver(time, game::wireOpcode(Opcode::MSG_MOVE_SET_FACING), true);
        }
        const bool pitched = (movement.flags & (flag(MovementFlags::SWIMMING) | flag(MovementFlags::FLYING))) != 0;
        if (pitched && intent.steeredPitchRad != movement.pitch) {
            movement.pitch = movement.anchorPitch = intent.steeredPitchRad;
            movement.pitchSin = std::sin(movement.pitch);
            movement.pitchCos = std::cos(movement.pitch);
            (void)deliver(time, game::wireOpcode(Opcode::MSG_MOVE_SET_PITCH), true);
        }
    }

    /// What the collision changed - a landing, entering or leaving water -
    /// told the server as a movement message. The original's collision-state
    /// handler is not ported or verified; this sends the message each change
    /// names, at the time the change happened.
    void reportStateChanges(const std::vector<collision::MovementRequestEvent>& events) {
        for (const auto& event : events) {
            if (event.kind != collision::MovementRequestEvent::Kind::StateChanged) continue;
            const uint32_t before = event.oldFlags, after = movement.flags;
            const auto changed = [&](MovementFlags f) {
                return ((before ^ after) & flag(f)) != 0;
            };
            if (changed(MovementFlags::FALLING) && !(after & flag(MovementFlags::FALLING))) {
                (void)deliver(event.time, game::wireOpcode(Opcode::MSG_MOVE_FALL_LAND), true);
            }
            if (changed(MovementFlags::SWIMMING)) {
                const bool swimming = (after & flag(MovementFlags::SWIMMING)) != 0;
                (void)deliver(event.time, game::wireOpcode(swimming ? Opcode::MSG_MOVE_START_SWIM
                                                                    : Opcode::MSG_MOVE_STOP_SWIM), true);
            }
        }
    }

    void walk(bool walking, uint32_t time) {
        const bool now = (movement.flags & flag(MovementFlags::WALKING)) != 0;
        if (walking == now) return;
        movement.flags ^= flag(MovementFlags::WALKING);
        (void)deliver(time, game::wireOpcode(walking ? Opcode::MSG_MOVE_SET_WALK_MODE : Opcode::MSG_MOVE_SET_RUN_MODE),
                      true);
    }
};

OriginalPlayerMover::OriginalPlayerMover(const CVarStore& settings)
    : state_(std::make_unique<State>(settings)) {}

OriginalPlayerMover::~OriginalPlayerMover() = default;

void OriginalPlayerMover::connect(collision::MovementWorld& world, game::GameHandler& game,
                                  const pipeline::GameTables& tables) {
    state_->world = &world;
    state_->game = &game;
    state_->tables = &tables;
}

bool OriginalPlayerMover::movesCharacter(const glm::vec3& position, float facingDeg) {
    auto& s = *state_;
    std::string why = s.unsupported(position);
    if (why.empty() && !s.active && !s.fitShape()) why = "the player's model has no collision box";
    s.say(why);
    if (!why.empty()) {
        s.active = false;
        return false;
    }
    if (!s.active) {
        s.seat(position, facingDeg);
        s.active = true;
    } else if (glm::distance(position, s.lastPosition) > 0.01f) {
        // Something else put the player somewhere: a teleport, the server
        // correcting the position. The original starts again from there.
        LOG_INFO("Original movement: the player was moved from (", s.lastPosition.x, ", ", s.lastPosition.y,
                 ", ", s.lastPosition.z, ") to (", position.x, ", ", position.y, ", ", position.z,
                 ") - starting again there");
        s.seat(position, facingDeg);
    }
    return true;
}

rendering::CharacterMotion OriginalPlayerMover::move(const rendering::CharacterIntent& intent) {
    auto& s = *state_;
    const uint32_t now = s.game->movement().movementClockMs();
    (void)s.fitShape();
    s.walk(intent.walk, now);
    s.steer(intent, now);
    s.queueInput(intent, now);
    const auto result = collision::executeUninterpolatedMovement(*s.world, s.cache, s.movement, s.shape, s.commands,
                                                                 s.clock, s, now, s.lastTime, true, true, false,
                                                                 false);
    switch (result.movement.geometryStatus) {
    case collision::MovementGeometryStatus::Failed:
        LOG_ERROR("Original movement: the collision at (", s.movement.position.x, ", ", s.movement.position.y,
                  ", ", s.movement.position.z, ") failed - the player stays still this frame and is the "
                  "camera's again in this area");
        s.failedTile = tileKey(s.movement.position);
        break;
    case collision::MovementGeometryStatus::NotReady:
        if (!s.waitingForMap) {
            s.waitingForMap = true;
            LOG_WARNING("Original movement: the map around the player is still loading - the player waits for it");
        }
        break;
    case collision::MovementGeometryStatus::Ready:
        if (s.waitingForMap) LOG_INFO("Original movement: the map around the player has loaded");
        s.waitingForMap = false;
        break;
    }
    s.reportStateChanges(result.movement.events);
    (void)flushTimedMovementReports12340(s.movement, s.reporting, s.clock, s, s.game->getPlayerGuid(), true, now);
    s.lastTime = now;
    s.lastPosition = s.movement.position;

    rendering::CharacterMotion motion;
    motion.position = s.movement.position;
    motion.facingDeg = glm::degrees(s.movement.facing);
    motion.swimming = (s.movement.flags & flag(MovementFlags::SWIMMING)) != 0;
    motion.flying = (s.movement.flags & flag(MovementFlags::FLYING)) != 0;
    motion.grounded = (s.movement.flags & flag(MovementFlags::FALLING)) == 0 && !motion.swimming && !motion.flying;
    motion.verticalVelocity = (s.movement.flags & flag(MovementFlags::FALLING)) ? -s.movement.fallVelocity : 0.0f;
    return motion;
}

bool OriginalPlayerMover::reportsMovement() const { return state_->active; }

bool OriginalPlayerMover::takeForcedMovement(const game::ForcedMovement& command) {
    auto& s = *state_;
    if (!s.active) return false;
    const uint32_t now = s.game->movement().movementClockMs();
    if (const auto rate = rateFor(command.acknowledgement)) {
        s.commands.enqueueRateChange(now, *rate, command.value, command.counter, true);
    } else if (const auto mode = modeFor(command)) {
        s.commands.enqueueMode(now, command.counter, mode->first, mode->second);
    } else if (command.acknowledgement == Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK) {
        s.commands.enqueueCollisionHeight(now, command.value, command.counter, true);
    } else if (command.acknowledgement == Opcode::CMSG_MOVE_KNOCK_BACK_ACK && command.launch) {
        s.commands.enqueueLaunch(now, command.counter,
                                 glm::vec2(command.launch->directionX, command.launch->directionY),
                                 command.launch->speed, command.launch->verticalSpeed);
    } else {
        if (s.unownedCommands.insert(static_cast<int>(command.acknowledgement)).second) {
            LOG_WARNING("Original movement: forced movement acknowledged by 0x", std::hex,
                        game::wireOpcode(command.acknowledgement), std::dec,
                        " has no place in its queue - the game applies and acknowledges it");
        }
        return false;
    }
    return true;
}

}  // namespace wowee::core
