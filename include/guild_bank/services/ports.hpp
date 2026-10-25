#pragma once

#include <cstdint>
#include <string>

namespace wowee::guild_bank {

/// Requests to the server about the vault. Implemented by the protocol layer.
class IGuildBankGateway {
public:
    virtual ~IGuildBankGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;

    /// Registers this session with the banker, so the server answers the
    /// tab queries that follow. Without it no bank list ever arrives.
    virtual void sendActivate(uint64_t banker) = 0;
    virtual void sendQueryTab(uint64_t banker, uint8_t tab) = 0;
    virtual void sendBuyTab(uint64_t banker, uint8_t tab) = 0;
    virtual void sendDepositMoney(uint64_t banker, uint32_t amount) = 0;
    virtual void sendWithdrawMoney(uint64_t banker, uint32_t amount) = 0;
    virtual void sendWithdrawItem(uint64_t banker, uint8_t tab, uint8_t bankSlot,
                                  uint8_t destBag, uint8_t destSlot, uint32_t splitCount) = 0;
    virtual void sendDepositItem(uint64_t banker, uint8_t tab, uint8_t bankSlot,
                                 uint8_t srcBag, uint8_t srcSlot) = 0;
    virtual void sendQueryText(uint8_t tab) = 0;
    virtual void sendTabText(uint8_t tab, const std::string& text) = 0;
    virtual void sendUpdateTab(uint64_t banker, uint8_t tab, const std::string& name, const std::string& icon) = 0;
    virtual void sendLogQuery(uint8_t tab) = 0;
};

/// What the vault needs from the rest of the game.
class IGuildBankWorld {
public:
    virtual ~IGuildBankWorld() = default;

    [[nodiscard]] virtual bool inGuild() const = 0;
    virtual void requestItem(uint32_t itemEntry) = 0;
};

} // namespace wowee::guild_bank
