#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::mail {

/// An item attached to a letter in the inbox.
struct MailAttachment {
    uint8_t slot = 0;
    uint32_t itemGuidLow = 0;
    uint32_t itemId = 0;
    uint32_t enchantId = 0;
    uint32_t randomPropertyId = 0;
    uint32_t randomSuffix = 0;
    uint32_t stackCount = 1;
    uint32_t chargesOrDurability = 0;
    uint32_t maxDurability = 0;
};

/// One letter in the inbox.
struct MailMessage {
    uint32_t messageId = 0;
    uint8_t messageType = 0;    // 0=normal, 2=auction, 3=creature, 4=gameobject
    uint64_t senderGuid = 0;
    uint32_t senderEntry = 0;   // For non-player mail
    std::string senderName;
    std::string subject;
    std::string body;
    uint32_t stationeryId = 0;
    uint64_t money = 0;
    uint64_t cod = 0;           // Cash on delivery
    uint32_t flags = 0;
    float expirationTime = 0.0f;
    uint32_t mailTemplateId = 0;
    bool read = false;
    std::vector<MailAttachment> attachments;
};

struct AuctionMailSubject {
    uint32_t itemEntry = 0;
    uint32_t response = 0;
    uint32_t lotId = 0;
    uint32_t itemCount = 0;
};

// Decoded auction-house mail body ("invoice"). The wire body is a colon-
// separated string the retail client never prints verbatim.
struct AuctionMailInvoice {
    uint32_t ownerGuidLow = 0;  // seller/buyer low GUID (hex field)
    uint32_t bid = 0;           // copper
    uint32_t buyout = 0;        // copper
    uint32_t deposit = 0;       // copper (successful-sale mail only)
    uint32_t consignment = 0;   // copper - auction house cut (successful sale)
};

/// An auction mail's subject: itemEntry:0:response:lotId:itemCount, or the
/// legacy itemEntry:0:response.
bool parseAuctionMailSubject(const std::string& subject, AuctionMailSubject& result);
/// The subject the way the client shows it: "Auction won: <item>" and so on.
std::string formatAuctionMailSubject(const AuctionMailSubject& subject,
                                     const std::string& itemName);
bool parseAuctionMailBody(const std::string& body, AuctionMailInvoice& result);

/// An item the player put on a letter they are writing.
struct Enclosure {
    uint64_t itemGuid = 0;
    uint32_t itemId = 0;
    uint32_t stackCount = 1;
    uint32_t displayInfoId = 0;
    std::string name;
    uint8_t srcBag = 0xFF;
    uint8_t srcSlot = 0;
    [[nodiscard]] bool occupied() const { return itemGuid != 0; }
};

/// The most a letter carries (TBC and WotLK); Vanilla's packet carries one.
inline constexpr int kMaxEnclosures = 12;

} // namespace wowee::mail
