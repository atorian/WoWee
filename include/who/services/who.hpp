#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::who {

struct WhoEntry;

/// /who as the interface and the addon layer use it.
class IWho {
public:
    virtual ~IWho() = default;

    [[nodiscard]] virtual const std::vector<WhoEntry>& getWhoResults() const = 0;
    [[nodiscard]] virtual uint32_t getWhoOnlineCount() const = 0;
    [[nodiscard]] virtual bool isWhoToUI() const = 0;
    /// Where answers go: the panel, or the chat (SetWhoToUI).
    virtual void setWhoToUI(bool toUI) = 0;
    virtual void queryWho(const std::string& playerName = "") = 0;
};

/// What the protocol layer tells /who.
class IWhoFeed {
public:
    virtual ~IWhoFeed() = default;

    /// SMSG_WHO: the count found, the count online, and the rows read.
    virtual void onAnswer(uint32_t found, uint32_t onlineCount, std::vector<WhoEntry> entries) = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::who
