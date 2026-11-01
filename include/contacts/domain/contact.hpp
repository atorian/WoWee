#pragma once

#include <cstdint>
#include <string>

namespace wowee::contacts {

/// Which lists a contact is on. One player can be on several at once.
namespace ContactFlag {
inline constexpr uint32_t Friend  = 0x1;
inline constexpr uint32_t Ignored = 0x2;
inline constexpr uint32_t Muted   = 0x4;
}

/// A player on the friend, ignore or mute list, as the server last described
/// them. The server owns the lists; this is the client's copy.
struct Contact {
    uint64_t    guid    = 0;
    std::string name;            // empty until the name query answers
    std::string note;
    uint32_t    flags   = 0;     // ContactFlag bits
    /// AzerothCore's FriendStatus, and a bitmask rather than a number:
    /// 0x01 online, 0x02 away, 0x04 do-not-disturb, 0x08 recruit-a-friend.
    /// An away friend is 0x03 - online *and* away - which is why this cannot
    /// be compared for equality. It used to be documented as 0/1/2/3 and read
    /// that way, so an away friend was shown as busy.
    uint8_t     status  = 0;
    uint32_t    areaId  = 0;
    uint32_t    level   = 0;
    uint32_t    classId = 0;

    [[nodiscard]] bool isFriend()  const { return (flags & ContactFlag::Friend) != 0; }
    [[nodiscard]] bool isIgnored() const { return (flags & ContactFlag::Ignored) != 0; }
    [[nodiscard]] bool isMuted()   const { return (flags & ContactFlag::Muted) != 0; }
    [[nodiscard]] bool isOnline()  const { return (status & 0x01) != 0; }
    [[nodiscard]] bool isAway()    const { return (status & 0x02) != 0; }
    [[nodiscard]] bool isBusy()    const { return (status & 0x04) != 0; }
};

/// One row of a contact list as the server sent it, already decoded.
struct ContactRecord {
    uint64_t    guid    = 0;
    uint32_t    flags   = 0;
    std::string note;
    uint8_t     status  = 0;
    uint32_t    areaId  = 0;
    uint32_t    level   = 0;
    uint32_t    classId = 0;
    std::string name;            // from the name cache, if it was there
};

/// AzerothCore's FriendsResult: not a state but the outcome of something that
/// happened to one friend. Read off SocialMgr.h rather than guessed - an
/// earlier numbering was five places out, so adding a friend announced that
/// the list was full and a database error deleted the friend.
enum class FriendResult : uint8_t {
    DbError      = 0,
    ListFull     = 1,
    Online       = 2,
    Offline      = 3,
    NotFound     = 4,
    Removed      = 5,
    AddedOnline  = 6,
    AddedOffline = 7,
    Already      = 8,
    Self         = 9,
    Enemy        = 10,
};

/// SMSG_FRIEND_STATUS, decoded. `result` stays a raw byte: the ignore and mute
/// results above Enemy arrive on the same packet and change nothing here.
struct FriendStatusUpdate {
    uint8_t     result   = 0;
    uint64_t    guid     = 0;
    std::string note;
    uint8_t     chatFlag = 0;    // FriendStatus bits, present when online
    uint32_t    areaId   = 0;
    uint32_t    level    = 0;
    uint32_t    classId  = 0;
};

} // namespace wowee::contacts
