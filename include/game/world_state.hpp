#pragma once

namespace wowee::game {

/**
 * World connection state
 */
enum class WorldState {
    DISCONNECTED,           // Not connected
    CONNECTING,             // TCP connection in progress
    CONNECTED,              // Connected, waiting for challenge
    CHALLENGE_RECEIVED,     // Received SMSG_AUTH_CHALLENGE
    AUTH_SENT,              // Sent CMSG_AUTH_SESSION, encryption initialized
    AUTHENTICATED,          // Received SMSG_AUTH_RESPONSE success
    READY,                  // Ready for character/world operations
    CHAR_LIST_REQUESTED,    // CMSG_CHAR_ENUM sent
    CHAR_LIST_RECEIVED,     // SMSG_CHAR_ENUM received
    ENTERING_WORLD,         // CMSG_PLAYER_LOGIN sent
    IN_WORLD,               // In game world
    FAILED                  // Connection or authentication failed
};

/// The state's own name, for a log line.
///
/// Four files kept an identical copy of this in their own anonymous namespace -
/// the three GameHandler was split into and the entity controller - so adding a
/// state meant remembering four switches, and a switch that was not updated
/// answers "UNKNOWN" for the new one rather than failing to compile.
///
/// Beside the enum, so the two are read together. No default case, so adding a
/// state to the list above is a warning here rather than a silent "UNKNOWN".
inline const char* worldStateName(WorldState state) {
    switch (state) {
        case WorldState::DISCONNECTED:        return "DISCONNECTED";
        case WorldState::CONNECTING:          return "CONNECTING";
        case WorldState::CONNECTED:           return "CONNECTED";
        case WorldState::CHALLENGE_RECEIVED:  return "CHALLENGE_RECEIVED";
        case WorldState::AUTH_SENT:           return "AUTH_SENT";
        case WorldState::AUTHENTICATED:       return "AUTHENTICATED";
        case WorldState::READY:               return "READY";
        case WorldState::CHAR_LIST_REQUESTED: return "CHAR_LIST_REQUESTED";
        case WorldState::CHAR_LIST_RECEIVED:  return "CHAR_LIST_RECEIVED";
        case WorldState::ENTERING_WORLD:      return "ENTERING_WORLD";
        case WorldState::IN_WORLD:            return "IN_WORLD";
        case WorldState::FAILED:              return "FAILED";
    }
    return "UNKNOWN";
}

} // namespace wowee::game
