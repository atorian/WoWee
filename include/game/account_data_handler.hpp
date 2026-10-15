#pragma once

#include "game/account_data.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

/// The account data: the eight slots the server keeps for the account and for
/// the character logged in - settings, key bindings, macros, chat and window
/// layout - and the macros read out of two of them.
///
/// Kept in step with the server as the original client keeps it. The server
/// says when each slot last changed (SMSG_ACCOUNT_DATA_TIMES); a slot newer
/// there than here is asked for, and one newer here - changed while the server
/// could not be told - is sent. Every slot is also kept on disk, the copy the
/// comparison is made against and the only copy on a server that keeps none.
///
/// A slot that changes here (a macro edited, a binding saved) is written to
/// disk and sent at once, with the time of the change.
class AccountDataHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit AccountDataHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Session, told by GameHandler ---------------------------------------

    /// A new connection, for this account on this server. Empties every slot.
    void beginSession(const std::string& accountName, const std::string& serverHost);
    /// The world server accepted the account: read its slots from disk and
    /// ask the server when its own copies changed.
    void accountAuthenticated();
    /// A character is being logged in: read its slots from disk. The server
    /// says when its copies changed once the login is under way.
    void characterSelected(uint64_t characterGuid);
    /// The character is in the world, so the server can store its slots:
    /// send the ones that changed before it could.
    void worldEntered();

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] const std::string& text(AccountDataType type) const;

    /// One half of the macros - the account's or the character's - in the
    /// order the macro frame lists them, which is by name.
    [[nodiscard]] const std::vector<Macro>& macros(bool character) const;
    [[nodiscard]] const Macro* macro(uint32_t id) const;

    // --- Commands ------------------------------------------------------------

    /// Replace a slot's text: kept, written to disk and sent to the server.
    void store(AccountDataType type, std::string text);

    /// A new macro in the account's half or the character's. Its id, or 0 when
    /// that half is full or no character is logged in to own it.
    uint32_t createMacro(bool character, const std::string& name,
                         const std::string& icon, const std::string& body);
    /// Change what is given; a null part is left as it is. False for an id
    /// that is not a macro.
    bool editMacro(uint32_t id, const std::string* name, const std::string* icon,
                   const std::string* body);
    /// Remove a macro, and take it off every action button that held it.
    bool deleteMacro(uint32_t id);

    /// Macros an older build of this client kept in its own character file.
    /// Each joins its half unless a macro of the same name is already there -
    /// now and again whenever the server's copy of that half arrives, so the
    /// server's copy cannot drop them before they have reached it.
    void adoptLegacyMacros(std::vector<Macro> macros);

private:
    struct Slot {
        std::string text;
        uint32_t time = 0;       // when it last changed, Unix time; 0 for never
        bool requested = false;  // asked of the server, answer not yet in
        bool unsent = false;     // newer here than the server has been told
    };

    void handleAccountDataTimes(network::Packet& packet);
    void handleUpdateAccountData(network::Packet& packet);
    void handleUpdateAccountDataComplete(network::Packet& packet);

    /// Whether the server in use keeps account data at all. WotLK's does;
    /// for the others the disk is the only copy.
    [[nodiscard]] bool serverKeepsAccountData() const;
    [[nodiscard]] bool slotInScope(AccountDataType type) const;
    [[nodiscard]] std::string cacheDir(AccountDataType type) const;
    void loadFromCache(AccountDataType type);
    void writeCache(AccountDataType type);
    void send(AccountDataType type);
    /// The slot holds new text: read the macros out of it if it holds them,
    /// and say so to the rest of the client.
    void loaded(AccountDataType type);
    void storeMacros(bool character);
    /// Add the pending legacy macros a half lacks; true if any were.
    bool mergeLegacyMacros(bool character);
    /// Said once per key - a failure that repeats every login is one line.
    void warnOnce(const std::string& key, const std::string& message);

    GameHandler& owner_;
    std::string accountKey_;
    uint64_t characterGuid_ = 0;
    std::array<Slot, kAccountDataTypeCount> slots_{};
    std::array<std::vector<Macro>, 2> macros_{};
    std::array<std::vector<Macro>, 2> legacyMacros_{};
    std::unordered_set<std::string> warned_;
};

} // namespace game
} // namespace wowee
