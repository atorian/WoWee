#include "game/realm_time_handler.hpp"

#include "core/logger.hpp"
#include "game/game_events.hpp"
#include "game/opcodes.hpp"
#include "game/packed_time.hpp"
#include "network/packet.hpp"

namespace wowee::game {

RealmTimeHandler::RealmTimeHandler(core::EventBus& events, std::function<Clock::time_point()> now)
    : now_(std::move(now)) {
    subscriptions_.push_back(events.subscribe<events::ServerTimeReceived>(
        [this](const events::ServerTimeReceived& e) {
            takeUnixTime(e.unixSeconds, "SMSG_QUERY_TIME_RESPONSE");
        }));
}

void RealmTimeHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_LOGIN_SETTIMESPEED] = [this](network::Packet& p) { handleLoginSetTimeSpeed(p); };
    table[Opcode::SMSG_GAMETIME_SET] = [this](network::Packet& p) { handleGameTime(p); };
    table[Opcode::SMSG_GAMETIME_UPDATE] = [this](network::Packet& p) { handleGameTime(p); };
    table[Opcode::SMSG_GAMESPEED_SET] = [this](network::Packet& p) { handleGameSpeed(p); };
    table[Opcode::SMSG_SERVERTIME] = [this](network::Packet& p) { handleServerTime(p); };
}

void RealmTimeHandler::handleLoginSetTimeSpeed(network::Packet& packet) {
    // SMSG_LOGIN_SETTIMESPEED (0x042)
    // Structure: PackedTime gameTime, float timeScale
    //
    // gameTime is a packed bitfield, not a count of seconds - Player.cpp writes
    // it with AppendPackedTime. It was being stored raw and then divided by
    // 86400 to get a time of day, which made the sky's clock a number with no
    // relation to the hour the server sent.
    //
    // timeScale: game minutes per real second - 1/60 is real time.
    if (packet.getSize() < 8) {
        LOG_WARNING("SMSG_LOGIN_SETTIMESPEED: packet too small (", packet.getSize(),
                    " bytes); the realm's time is not known from it");
        return;
    }

    const WowDate now = unpackWowPackedTime(packet.readUInt32());
    const float timeScale = packet.readFloat();

    gameClock_.set(static_cast<float>(now.hour) + static_cast<float>(now.minute) / 60.0f,
                   timeScale, now_());
    takePackedDate(now, "SMSG_LOGIN_SETTIMESPEED");

    LOG_INFO("Server time: ", now.hour, ":", now.minute, ", timeSpeed=", timeScale,
             " (1 game day = ", 24.0f / (gameClock_.speed() * 60.0f), " real hours)");
}

void RealmTimeHandler::handleGameTime(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        const WowDate t = unpackWowPackedTime(packet.readUInt32());
        gameClock_.setHours(static_cast<float>(t.hour) + static_cast<float>(t.minute) / 60.0f,
                            now_());
        takePackedDate(t, "SMSG_GAMETIME_SET/UPDATE");
        // Said out loud, because this is where realm time actually comes from
        // on servers that never send SMSG_LOGIN_SETTIMESPEED - which logged its
        // arrival and so looked like the only source. Chasing a frozen sky
        // through a log with no line for this in it led to exactly the wrong
        // conclusion.
        LOG_INFO("Realm time: ", t.hour, ":", t.minute);
    }
    packet.skipAll();
}

void RealmTimeHandler::handleGameSpeed(network::Packet& packet) {
    if (packet.hasRemaining(8)) {
        const WowDate t = unpackWowPackedTime(packet.readUInt32());
        const float timeSpeed = packet.readFloat();
        gameClock_.set(static_cast<float>(t.hour) + static_cast<float>(t.minute) / 60.0f,
                       timeSpeed, now_());
        takePackedDate(t, "SMSG_GAMESPEED_SET");
    }
    packet.skipAll();
}

void RealmTimeHandler::handleServerTime(network::Packet& packet) {
    // uint32 unixTime - the server's own wall clock. Not a time of day: it used
    // to be stored in the game clock alongside values in three other units, so
    // whichever packet arrived last decided what the sky thought the hour was.
    // It is the realm clock's.
    if (packet.hasRemaining(4)) {
        takeUnixTime(packet.readUInt32(), "SMSG_SERVERTIME");
    }
    packet.skipAll();
}

void RealmTimeHandler::takePackedDate(const WowDate& date, const char* from) {
    if (realmClock_.source() == RealmClock::Source::UnixTime) return;  // knows better already
    if (realmClock_.setPackedDate(date, now_())) {
        LOG_INFO("Realm clock: ", date.fullYear(), "-", date.month, "-", date.day, " ", date.hour,
                 ":", date.minute, " from ", from, ", to the minute until the server's Unix time "
                 "arrives");
        return;
    }
    if (!badPackedDateSaid_) {
        badPackedDateSaid_ = true;
        LOG_WARNING("Realm clock: ", from, " carried no date (", date.fullYear(), "-", date.month,
                    "-", date.day, " ", date.hour, ":", date.minute,
                    "); the realm clock waits for the server's Unix time");
    }
}

void RealmTimeHandler::takeUnixTime(uint32_t unixSeconds, const char* from) {
    if (unixSeconds == 0) {
        LOG_WARNING("Realm clock: ", from, " carried a Unix time of zero; ignored, the realm clock "
                    "keeps what it had");
        return;
    }
    const bool first = realmClock_.source() != RealmClock::Source::UnixTime;
    realmClock_.setUnixTime(unixSeconds, now_());
    if (first) LOG_INFO("Realm clock: Unix time ", unixSeconds, " from ", from);
}

}  // namespace wowee::game
