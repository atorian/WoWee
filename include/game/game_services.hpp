#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game { class ExpansionRegistry; class EmoteTable; class MovementCollision; }
namespace core { class EventBus; class CVarStore; }
namespace contacts { class IContactsServerFeed; }
namespace who { class IWhoFeed; }
namespace duel { class IDuelsFeed; }
namespace petition { class IPetitionsFeed; }
namespace arena { class IArenaTeamsFeed; }
namespace battleground { class IBattlegroundsFeed; }
namespace lfg { class ILfgFeed; }
namespace calendar { class ICalendarFeed; }
namespace group { class IGroupServerFeed; class IGroup; }
namespace guild { class IGuildServerFeed; class IGuild; }
namespace auction { class IAuctionHouseFeed; }
namespace mail { class IMailboxFeed; class IMailbox; }
namespace guild_bank { class IGuildBankFeed; class IGuildBank; }
namespace network { class WorldConnection; }

namespace game {

// Explicit service dependencies for game handlers.
// Owned by Application, passed by reference to GameHandler at construction.
// Replaces hidden Application::getInstance() singleton access.
//
// Nothing here draws or sounds: the game says what happened on `events`, and
// the client's presentation (core/) shows and plays it. See AGENTS.md,
// "Layers".
struct GameServices {
    pipeline::AssetManager* assetManager = nullptr;
    ExpansionRegistry* expansionRegistry = nullptr;
    /// What the emotes are. Required, like `events`.
    const EmoteTable* emotes = nullptr;
    /// Where the game announces what happened. Required: GameHandler
    /// refuses to be built without one.
    core::EventBus* events = nullptr;
    /// The CVars the game's own behaviour follows - autoClearAFK,
    /// assistAttack, autoDismountFlying. Required, like `events`; the client
    /// owns the store and the interface writes the same one.
    const core::CVarStore* cvars = nullptr;
    /// A new connection to a world server, one per login. Required: the
    /// client makes a network::WorldSocket, a test one it can play packets
    /// into and read what was sent from.
    std::function<std::unique_ptr<network::WorldConnection>()> makeConnection;
    // The walls units moving themselves run into between their reports.
    const MovementCollision* movementCollision = nullptr;
    /// Where the account data - key bindings, macros, settings - is kept on
    /// disk, one folder per account and server. Empty keeps none, which
    /// AccountDataHandler says once.
    std::string accountDataDir;
    /// The monotonic clock the realm's time is kept running on between the
    /// packets that set it. Real time in the client; a test hands over its
    /// own, so time passes when it says.
    std::function<std::chrono::steady_clock::time_point()> steadyNow =
        [] { return std::chrono::steady_clock::now(); };
    /// The contact lists, which the friend, ignore and mute packets feed.
    /// Set by the composition root once the game exists: the contacts
    /// gateway sends through it.
    contacts::IContactsServerFeed* contactsFeed = nullptr;
    /// Where SMSG_WHO's answer goes. Set like contactsFeed.
    who::IWhoFeed* whoFeed = nullptr;
    /// Where the duel packets go. Set like contactsFeed.
    duel::IDuelsFeed* duelFeed = nullptr;
    /// Where the charter packets go. Set like contactsFeed.
    petition::IPetitionsFeed* petitionFeed = nullptr;
    /// Where the arena team packets go. Set like contactsFeed.
    arena::IArenaTeamsFeed* arenaFeed = nullptr;
    /// Where the battleground packets go. Set like contactsFeed.
    battleground::IBattlegroundsFeed* battlegroundFeed = nullptr;
    /// Where the dungeon finder's packets go. Set like contactsFeed.
    lfg::ILfgFeed* lfgFeed = nullptr;
    /// Where the calendar's packets go. Set like contactsFeed.
    calendar::ICalendarFeed* calendarFeed = nullptr;
    /// Where the party and raid packets go, and the group as it stands for
    /// the handlers that ask who is in it. Set like contactsFeed.
    group::IGroupServerFeed* groupFeed = nullptr;
    group::IGroup* group = nullptr;
    /// Where the guild's packets go, and the player's guild for the handlers
    /// that read its roster.
    guild::IGuildServerFeed* guildFeed = nullptr;
    guild::IGuild* guild = nullptr;
    /// Where the auction packets and the client's clock go. Set like
    /// contactsFeed.
    auction::IAuctionHouseFeed* auctionFeed = nullptr;
    /// Where the mail packets go, and the mailbox a used mailbox opens.
    mail::IMailboxFeed* mailFeed = nullptr;
    mail::IMailbox* mail = nullptr;
    /// Where the vault's packets and the guild's balance go, and the vault a
    /// used banker opens.
    guild_bank::IGuildBankFeed* guildBankFeed = nullptr;
    guild_bank::IGuildBank* guildBank = nullptr;
    uint32_t gryphonDisplayId = 0;
    uint32_t wyvernDisplayId = 0;
};

} // namespace game
} // namespace wowee
