#pragma once

#include "mail/domain/records.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::mail {

/// Requests to the server about mail. Implemented by the protocol layer.
class IMailGateway {
public:
    virtual ~IMailGateway() = default;

    [[nodiscard]] virtual bool inWorld() const = 0;
    [[nodiscard]] virtual bool connected() const = 0;
    /// Enclosures this realm's letter can carry: one on Vanilla.
    [[nodiscard]] virtual int maxEnclosures() const = 0;

    virtual void sendListRequest(uint64_t mailbox) = 0;
    virtual void sendLetter(uint64_t mailbox, const std::string& recipient, const std::string& subject,
                            const std::string& body, uint64_t money, uint64_t cod,
                            const std::vector<uint64_t>& itemGuids) = 0;
    virtual void sendTakeMoney(uint64_t mailbox, uint32_t mailId) = 0;
    virtual void sendTakeItem(uint64_t mailbox, uint32_t mailId, uint32_t itemGuidLow) = 0;
    virtual void sendReturn(uint64_t mailbox, uint32_t mailId) = 0;
    virtual void sendDelete(uint64_t mailbox, uint32_t mailId) = 0;
    virtual void sendMarkRead(uint64_t mailbox, uint32_t mailId) = 0;
};

/// What the mailbox needs from the rest of the game.
class IMailWorld {
public:
    virtual ~IMailWorld() = default;

    /// The item in a backpack slot or a worn bag's slot, ready to enclose.
    [[nodiscard]] virtual std::optional<Enclosure> backpackItem(int index) const = 0;
    [[nodiscard]] virtual std::optional<Enclosure> bagItem(int bag, int slot) const = 0;

    enum class Refund { Unknown, Open, Over };
    /// Whether an item can still be returned to its vendor. Unknown asks the
    /// server, so the next time is known.
    virtual Refund refundWindow(uint64_t itemGuid) = 0;

    /// Chooses the paper the first letter is written on: FrameXML enables
    /// Send only once one is chosen, and nothing chooses one as it opens.
    virtual void chooseDefaultStationery() = 0;

    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    [[nodiscard]] virtual std::string creatureName(uint32_t entry) const = 0;
    [[nodiscard]] virtual std::string gameObjectName(uint32_t entry) const = 0;
    /// The item's name, empty while its template has not arrived.
    [[nodiscard]] virtual std::string itemName(uint32_t itemEntry) const = 0;
    virtual void requestItem(uint32_t itemEntry) = 0;
    virtual void requestPlayerName(uint64_t guid) = 0;
};

} // namespace wowee::mail
