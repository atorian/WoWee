#pragma once

#include "game/opcode_table.hpp"
#include "pipeline/weather_kind.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace wowee::core { class EventBus; }
namespace wowee::network { class Packet; }

namespace wowee::game {

class ChatLog;
class FrameXmlEvents;

/// The weather where the character stands, as the realm last said it.
struct WeatherReport {
    pipeline::WeatherKind kind = pipeline::WeatherKind::Clear;
    /// How hard, 0-1: the realm's grade for the weather it names.
    float intensity = 0.0f;
    /// The number the realm sent, in its own numbering (see decodeWeather).
    uint32_t realmState = 0;
    /// The realm asked for this weather to arrive at once rather than to
    /// come over.
    bool abrupt = false;
};

/// How a realm numbers its weather in SMSG_WEATHER.
enum class WeatherNumbering {
    /// 1.12: the WeatherType the realm keeps - 0 fine, 1 rain, 2 snow,
    /// 3 storm, which is a sandstorm (Tanaris and Silithus are the zones that
    /// have it), and 86 thunder and 90 black rain.
    Classic,
    /// TBC and WotLK: a WeatherState that says the grade in the number -
    /// 3/4/5 light, medium and heavy rain, 6/7/8 snow, 22/41/42 sandstorm,
    /// 86 thunder, 90 black rain, 106 black snow, 1 fog and 2 drizzle
    /// (TrinityCore and AzerothCore, Weather.h).
    WeatherState,
};

/// The kind a realm's weather number stands for, or empty for a number that
/// is none of them.
[[nodiscard]] std::optional<pipeline::WeatherKind> decodeWeather(uint32_t realmState,
                                                                 WeatherNumbering numbering);

/// The weather: SMSG_WEATHER in, the report the client draws and sounds from
/// out, and events::WeatherChanged when it changes.
///
/// The realm decides the weather, and only the realm: with nothing said it is
/// clear. Each new character starts clear until the realm says otherwise for
/// the zone it enters in.
class WeatherHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    /// `numbering` is asked at each packet: the realm's expansion is chosen
    /// after the game is built.
    WeatherHandler(core::EventBus& events, FrameXmlEvents& frameXml, ChatLog& chat,
                   std::function<WeatherNumbering()> numbering);

    void registerOpcodes(DispatchTable& table);

    /// A vanilla-family server's 0x006B, which some of them send as
    /// SMSG_WEATHER: taken as weather when it is shaped like one - a known
    /// weather number, an intensity in range and an abrupt flag - and left
    /// unread otherwise for the opcode's other meaning. Whether it was taken.
    bool takeAliasedWeather(network::Packet& packet);

    /// A character is entering the world: clear, until the realm says.
    void beginCharacter();

    [[nodiscard]] const WeatherReport& current() const { return current_; }

private:
    void handleWeather(network::Packet& packet);
    /// The realm said this weather, in this numbering.
    void apply(uint32_t realmState, float intensity, bool abrupt, WeatherNumbering numbering,
               const char* from);

    core::EventBus& events_;
    FrameXmlEvents& frameXml_;
    ChatLog& chat_;
    std::function<WeatherNumbering()> numbering_;
    WeatherReport current_;
    /// Weather numbers that were none we know, said once each.
    std::unordered_set<uint32_t> unknownSaid_;
};

}  // namespace wowee::game
