#pragma once

#include <variant>

namespace wowee::guild_bank {

// What happened at the vault.
struct VaultOpened {};
struct VaultClosed {};
/// The set of tabs changed (GUILDBANK_UPDATE_TABS).
struct TabsChanged {};
/// A tab's slots changed (GUILDBANKBAGSLOTS_CHANGED).
struct SlotsChanged {};
struct MoneyChanged {};
struct WithdrawAllowanceChanged {};
/// `tab` counts from one, as the panel does.
struct TabTextChanged { int tab = 0; };
struct LogChanged {};

using VaultChange = std::variant<VaultOpened, VaultClosed, TabsChanged, SlotsChanged,
                                 MoneyChanged, WithdrawAllowanceChanged, TabTextChanged, LogChanged>;

// What became of a request the player made, published by the service.

/// A vault asked for by a player in no guild. The server answers that with an
/// error and no list, so nothing would ever have opened.
struct OpenRefused {};
/// A deposit from the bags into a tab with no empty slot.
struct TabFull {};

} // namespace wowee::guild_bank
