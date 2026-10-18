#include "game/weather_handler.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_events.hpp"
#include "game/opcodes.hpp"
#include "network/packet.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace wowee::game {

using pipeline::WeatherKind;

std::optional<WeatherKind> decodeWeather(uint32_t realmState, WeatherNumbering numbering) {
    if (numbering == WeatherNumbering::Classic) {
        switch (realmState) {
            case 0: return WeatherKind::Clear;
            case 1: return WeatherKind::Rain;
            case 2: return WeatherKind::Snow;
            case 3: return WeatherKind::Sandstorm;
            case 86: return WeatherKind::Thunderstorm;
            case 90: return WeatherKind::BlackRain;
            default: return std::nullopt;
        }
    }
    switch (realmState) {
        case 0: return WeatherKind::Clear;
        case 1: return WeatherKind::Fog;
        case 2:  // drizzle
        case 3:
        case 4:
        case 5: return WeatherKind::Rain;
        case 6:
        case 7:
        case 8: return WeatherKind::Snow;
        case 22:
        case 41:
        case 42: return WeatherKind::Sandstorm;
        case 86: return WeatherKind::Thunderstorm;
        case 90: return WeatherKind::BlackRain;
        case 106: return WeatherKind::BlackSnow;
        default: return std::nullopt;
    }
}

namespace {

/// What the chat says as the weather turns to `kind`, or nothing.
const char* announcement(WeatherKind kind) {
    switch (kind) {
        case WeatherKind::Clear: return "The weather clears.";
        case WeatherKind::Fog: return "A mist rolls in.";
        case WeatherKind::Rain:
        case WeatherKind::BlackRain: return "It begins to rain.";
        case WeatherKind::Snow:
        case WeatherKind::BlackSnow: return "It begins to snow.";
        case WeatherKind::Sandstorm: return "A sandstorm rises.";
        case WeatherKind::Thunderstorm: return "A storm rolls in.";
    }
    return nullptr;
}

}  // namespace

WeatherHandler::WeatherHandler(core::EventBus& events, FrameXmlEvents& frameXml, ChatLog& chat,
                               std::function<WeatherNumbering()> numbering)
    : events_(events), frameXml_(frameXml), chat_(chat), numbering_(std::move(numbering)) {}

void WeatherHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_WEATHER] = [this](network::Packet& p) { handleWeather(p); };
}

void WeatherHandler::beginCharacter() {
    current_ = WeatherReport{};
}

void WeatherHandler::handleWeather(network::Packet& packet) {
    // uint32 weather, float intensity, and from TBC on uint8 abrupt. Some
    // vanilla-family servers send the byte too.
    if (!packet.hasRemaining(8)) {
        LOG_WARNING("SMSG_WEATHER: ", packet.getSize(), " bytes, too short for a weather and an "
                    "intensity - the weather stays as it was");
        packet.skipAll();
        return;
    }
    const uint32_t state = packet.readUInt32();
    const float intensity = packet.readFloat();
    const bool abrupt = packet.hasRemaining(1) && packet.readUInt8() != 0;
    apply(state, intensity, abrupt, numbering_(), "SMSG_WEATHER");
    packet.skipAll();
}

bool WeatherHandler::takeAliasedWeather(network::Packet& packet) {
    const size_t start = packet.getReadPos();
    if (!packet.hasRemaining(9)) return false;
    const uint32_t state = packet.readUInt32();
    const float intensity = packet.readFloat();
    const uint8_t abrupt = packet.readUInt8();
    const bool shaped = decodeWeather(state, WeatherNumbering::Classic).has_value() &&
                        std::isfinite(intensity) && intensity >= 0.0f && intensity <= 1.5f && abrupt <= 1;
    if (!shaped) {
        packet.setReadPos(start);
        return false;
    }
    // Shaped as a vanilla server writes it, so read in that numbering
    // whichever expansion the realm says it is.
    apply(state, intensity, abrupt != 0, WeatherNumbering::Classic, "0x006B as SMSG_WEATHER");
    return true;
}

void WeatherHandler::apply(uint32_t realmState, float intensity, bool abrupt, WeatherNumbering numbering,
                           const char* from) {
    const auto decoded = decodeWeather(realmState, numbering);
    if (!decoded) {
        if (unknownSaid_.insert(realmState).second) {
            LOG_WARNING(from, ": weather ", realmState, " is none this client knows - drawn as clear");
        }
    }
    const WeatherKind kind = decoded.value_or(WeatherKind::Clear);
    if (!std::isfinite(intensity) || intensity < 0.0f || intensity > 1.0f) {
        LOG_WARNING(from, ": weather intensity ", intensity, " is outside 0-1 - drawn at ",
                    std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 1.0f) : 0.0f);
        intensity = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 1.0f) : 0.0f;
    }

    const WeatherKind previous = current_.kind;
    current_ = WeatherReport{.kind = kind, .intensity = intensity, .realmState = realmState, .abrupt = abrupt};
    LOG_INFO("Weather: ", pipeline::weatherKindName(kind), " (realm's ", realmState, ") at ", intensity,
             abrupt ? ", at once" : "");

    if (kind != previous) {
        // The first weather of a session comes clear-to-clear for most zones
        // and says nothing; a zone entered in the rain says so.
        if (const char* line = announcement(kind)) chat_.addSystemChatMessage(line);
    }
    events_.publish(events::WeatherChanged{.kind = kind, .intensity = intensity, .abrupt = abrupt,
                                           .previous = previous});
    // Not one of 3.3.5's events; addons written for this client listen for
    // it, with the realm's own number.
    if (frameXml_.frameXmlListening()) {
        frameXml_.fireFrameXmlEvent("WEATHER_CHANGED", {std::to_string(realmState), std::to_string(intensity)});
    }
}

}  // namespace wowee::game
