#pragma once

#include "guild/domain/records.hpp"

#include <string>
#include <vector>

/// An SMSG_GUILD_EVENT as the protocol layer hands it over.
inline wowee::guild::ServerEvent guildEvent(uint8_t type, std::vector<std::string> strings = {}) {
    wowee::guild::ServerEvent e;
    e.type = type;
    e.numStrings = static_cast<uint8_t>(strings.size());
    for (size_t i = 0; i < strings.size() && i < 3; ++i) e.strings[i] = strings[i];
    return e;
}
