#pragma once

/// The account data the server keeps for a client: eight text blobs, each the
/// file the original client writes under WTF - config-cache.wtf,
/// bindings-cache.wtf, macros-cache.txt and the rest - with the time it was
/// last changed.
///
/// What is here is the vocabulary and the two formats this client reads and
/// writes itself, macros and key bindings. Who holds the blobs and keeps them
/// in step with the server is AccountDataHandler.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/// The eight slots, numbered as the protocol numbers them.
enum class AccountDataType : uint8_t {
    GlobalConfig      = 0,  // config-cache.wtf, account
    CharacterConfig   = 1,  // config-cache.wtf, character
    GlobalBindings    = 2,  // bindings-cache.wtf, account
    CharacterBindings = 3,  // bindings-cache.wtf, character
    GlobalMacros      = 4,  // macros-cache.txt, account
    CharacterMacros   = 5,  // macros-cache.txt, character
    CharacterLayout   = 6,  // layout-cache.txt
    CharacterChat     = 7,  // chat-cache.txt
};

inline constexpr int kAccountDataTypeCount = 8;

/// The slots that belong to the account, and those that belong to whichever
/// character is logged in - the two masks SMSG_ACCOUNT_DATA_TIMES carries.
inline constexpr uint32_t kGlobalAccountDataMask    = 0x15;
inline constexpr uint32_t kCharacterAccountDataMask = 0xEA;

[[nodiscard]] constexpr bool isCharacterAccountData(AccountDataType type) {
    return (kCharacterAccountDataMask >> static_cast<uint32_t>(type)) & 1u;
}

/// The file the original client keeps the slot in, for log lines a player can
/// match against their own WTF folder.
[[nodiscard]] const char* accountDataName(AccountDataType type);

// ---- Macros -----------------------------------------------------------------

/// A character's macro ids carry this bit; the account's are small numbers.
/// An action button stores the low 24 bits and says which half by its type
/// (0x40 for the account's, 0x41 for the character's), so the two never meet.
inline constexpr uint32_t kCharacterMacroIdBase = 0x01000000;
/// How many each half holds - the macro frame's two tabs.
inline constexpr int kMaxAccountMacros   = 36;
inline constexpr int kMaxCharacterMacros = 18;

struct Macro {
    uint32_t id = 0;
    std::string name;
    /// An icon's name as macros-cache.txt writes it, without the
    /// Interface\Icons\ folder: "Spell_Holy_ChampionsGrace".
    std::string icon;
    /// One command per line, joined with '\n'.
    std::string body;
};

/// Read macros-cache.txt:
///
///     MACRO 16777217 "up" INV_Misc_QuestionMark
///     /cast Bloodrage
///     END
///
/// Lines may end in CRLF, which is how the original client writes them. A
/// block that does not parse is skipped and described in `problems`, so the
/// caller can say which macros were lost and why.
[[nodiscard]] std::vector<Macro> parseMacrosCache(const std::string& text,
                                                  std::vector<std::string>* problems = nullptr);

/// Write macros-cache.txt in the original client's own form, CRLF and all, so
/// the text read back by it is the text it would have written.
[[nodiscard]] std::string writeMacrosCache(const std::vector<Macro>& macros);

// ---- Key bindings -----------------------------------------------------------

/// "bind KEY COMMAND". A command of NONE takes the key away from whatever had
/// it by default.
struct KeyBindingLine {
    std::string key;
    std::string command;
};

inline constexpr const char* kUnboundCommand = "NONE";

/// Read bindings-cache.wtf - a BINDINGMODE line and then the bindings, in the
/// order they are to be applied.
[[nodiscard]] std::vector<KeyBindingLine> parseBindingsCache(const std::string& text,
                                                             std::vector<std::string>* problems = nullptr);

[[nodiscard]] std::string writeBindingsCache(const std::vector<KeyBindingLine>& lines);

} // namespace game
} // namespace wowee
