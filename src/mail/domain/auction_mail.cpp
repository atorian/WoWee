#include "mail/domain/records.hpp"

#include <array>
#include <cctype>
#include <charconv>

namespace wowee::mail {

bool parseAuctionMailSubject(const std::string& subject, AuctionMailSubject& result) {
    std::array<uint32_t, 5> fields{};
    size_t start = 0;
    size_t fieldCount = 0;
    while (start <= subject.size() && fieldCount < fields.size()) {
        const size_t end = subject.find(':', start);
        const size_t fieldEnd = end == std::string::npos ? subject.size() : end;
        if (fieldEnd == start) return false;
        const char* first = subject.data() + start;
        const char* last = subject.data() + fieldEnd;
        auto [parsedEnd, error] = std::from_chars(first, last, fields[fieldCount]);
        if (error != std::errc{} || parsedEnd != last) return false;
        ++fieldCount;
        if (end == std::string::npos) break;
        start = fieldEnd + 1;
    }

    // Modern subjects are itemEntry:0:response:lotId:itemCount. Legacy MaNGOS
    // cores emitted only itemEntry:0:response.
    if (fieldCount != 3 && fieldCount != 5) return false;
    if (fields[0] == 0 || fields[1] != 0 || fields[2] > 6) return false;
    result = {.itemEntry = fields[0], .response = fields[2],
              .lotId = fieldCount == 5 ? fields[3] : 0,
              .itemCount = fieldCount == 5 ? fields[4] : 0};
    return true;
}

std::string formatAuctionMailSubject(const AuctionMailSubject& subject,
                                     const std::string& itemName) {
    static constexpr std::array<const char*, 7> kPrefixes = {
        "Outbid on ",
        "Auction won: ",
        "Auction successful: ",
        "Auction expired: ",
        "Auction cancelled: ",
        "Auction cancelled: ",
        "Sale Pending: ",
    };
    if (subject.response >= kPrefixes.size()) return itemName;
    return std::string(kPrefixes[subject.response]) + itemName;
}

// Auction-house mail carries a machine-readable invoice as its body:
//   "<hex ownerGuidLow>:<bid>:<buyout>[:<deposit>:<consignment>...]"
// The leading GUID field is right-justified to width 16, so it can arrive with
// leading spaces (e.g. "           88a79:6000:6000:0:0:0:0"). The retail client
// parses this into a formatted invoice rather than printing it verbatim.
bool parseAuctionMailBody(const std::string& body, AuctionMailInvoice& result) {
    std::array<uint32_t, 5> fields{};
    size_t fieldCount = 0;
    size_t start = 0;
    while (start <= body.size() && fieldCount < fields.size()) {
        // The GUID field is space-padded to width 16 - skip leading whitespace.
        while (start < body.size() &&
               std::isspace(static_cast<unsigned char>(body[start]))) {
            ++start;
        }
        const size_t end = body.find(':', start);
        const size_t fieldEnd = end == std::string::npos ? body.size() : end;
        if (fieldEnd == start) return false;
        const char* first = body.data() + start;
        const char* last = body.data() + fieldEnd;
        // Field 0 is the owner GUID in hex; the rest are decimal copper values.
        const int base = fieldCount == 0 ? 16 : 10;
        auto [parsedEnd, error] = std::from_chars(first, last, fields[fieldCount], base);
        if (error != std::errc{} || parsedEnd != last) return false;
        ++fieldCount;
        if (end == std::string::npos) break;
        start = fieldEnd + 1;
    }

    // Minimum invoice is ownerGuid:bid:buyout (auction-won mail); a successful
    // sale adds deposit and consignment. Any trailing fields (moneyDelay/eta on
    // some cores) are ignored.
    if (fieldCount < 3) return false;
    result.ownerGuidLow = fields[0];
    result.bid = fields[1];
    result.buyout = fields[2];
    result.deposit = fieldCount > 3 ? fields[3] : 0;
    result.consignment = fieldCount > 4 ? fields[4] : 0;
    return true;
}

} // namespace wowee::mail
