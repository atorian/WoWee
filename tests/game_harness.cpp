#include "game_harness.hpp"
#include "contacts/services/contacts_service.hpp"
#include "game/contacts_gateway.hpp"
#include "game/who_gateway.hpp"
#include "who/services/who_service.hpp"
#include "game/duel_gateway.hpp"
#include "duel/services/duel_service.hpp"
#include "game/petition_gateway.hpp"
#include "petition/services/petition_service.hpp"
#include "game/arena_gateway.hpp"
#include "arena/services/arena_service.hpp"
#include "game/battleground_gateway.hpp"
#include "battleground/services/battleground_service.hpp"
#include "game/lfg_gateway.hpp"
#include "lfg/services/lfg_service.hpp"
#include "game/calendar_gateway.hpp"
#include "calendar/services/calendar_service.hpp"
#include "game/group_gateway.hpp"
#include "group/services/group_service.hpp"
#include "game/guild_gateway.hpp"
#include "guild/services/guild_service.hpp"
#include "game/auction_gateway.hpp"
#include "auction/services/auction_service.hpp"
#include "game/mail_gateway.hpp"
#include "mail/services/mail_service.hpp"
#include "game/guild_bank_gateway.hpp"
#include "guild_bank/services/guild_bank_service.hpp"
#include "game/trade_gateway.hpp"
#include "trade/services/trade_service.hpp"
#include "game/loot_gateway.hpp"
#include "loot/services/loot_service.hpp"
#include "achievement/services/achievement_service.hpp"
#include "game/title_gateway.hpp"
#include "title/services/title_service.hpp"
#include "game/reputation_gateway.hpp"
#include "reputation/services/reputation_service.hpp"
#include "game/pet_gateway.hpp"
#include "pet/services/pet_service.hpp"

#include <catch_amalgamated.hpp>  // test_support.hpp's REQUIRE
#include "test_support.hpp"

#include "foundation/env.hpp"
#include "game/entity.hpp"
#include "game/world_handler.hpp"
#include "game/protocol_tables.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packet_data.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace wowee::test {

// ---- FakeWorldConnection ----------------------------------------------------

void FakeWorldConnection::beginConnect(const std::string&, uint16_t, int) {
    connecting_ = true;
}

net::AsyncConnect::Status FakeWorldConnection::pollConnect() {
    if (connected_) return net::AsyncConnect::Status::Connected;
    if (!connecting_) return net::AsyncConnect::Status::Idle;
    connecting_ = false;
    if (refused_) return net::AsyncConnect::Status::Failed;
    connected_ = true;
    return net::AsyncConnect::Status::Connected;
}

void FakeWorldConnection::send(const network::Packet& packet) {
    sent_.push_back(packet);
    if (!answer_ || !connected_) return;
    if (auto reply = answer_(packet)) inbound_.push_back(std::move(*reply));
}

void FakeWorldConnection::update() {
    while (!inbound_.empty() && callback_) {
        network::Packet packet = std::move(inbound_.front());
        inbound_.pop_front();
        callback_(packet);
    }
}

// ---- Body -------------------------------------------------------------------

Body& Body::u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
}

Body& Body::u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
}

Body& Body::f32(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return u32(bits);
}

Body& Body::cstr(const std::string& s) {
    bytes_.insert(bytes_.end(), s.begin(), s.end());
    bytes_.push_back(0);
    return *this;
}

Body& Body::packedGuid(uint64_t guid) {
    uint8_t mask = 0;
    std::vector<uint8_t> parts;
    for (int i = 0; i < 8; ++i) {
        const uint8_t byte = static_cast<uint8_t>(guid >> (8 * i));
        if (byte != 0) {
            mask |= static_cast<uint8_t>(1u << i);
            parts.push_back(byte);
        }
    }
    bytes_.push_back(mask);
    bytes_.insert(bytes_.end(), parts.begin(), parts.end());
    return *this;
}

// ---- GameHarness ------------------------------------------------------------

namespace {

/// No server extensions in a test: the game looks for one beside the client
/// for every host it connects to, and a machine with Data/server_extensions
/// filled in must not change what a test sees.
void useEmptyExtensionsDir() {
    const std::string dir = scratchDir("wowee_harness_extensions").string();
    core::setEnvVar("WOWEE_SERVER_EXTENSIONS_DIR", dir.c_str());
}

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("GameHarness: " + what);
}

/// Guild names for one test: the client keeps them in the user's own file,
/// which a test must neither read nor write.
class MemoryGuildNames final : public guild::IGuildNameStore {
public:
    std::unordered_map<uint32_t, std::string> load() const override { return names_; }
    void save(const std::unordered_map<uint32_t, std::string>& names) override { names_ = names; }

private:
    std::unordered_map<uint32_t, std::string> names_;
};

} // namespace

GameHarness::GameHarness() : GameHarness(scratchDir("wowee_harness_account_data")) {}

GameHarness::GameHarness(game::ClientMover& clientMover)
    : GameHarness(scratchDir("wowee_harness_account_data"), &clientMover) {}

GameHarness::GameHarness(const std::filesystem::path& accountDataDir)
    : GameHarness(accountDataDir, nullptr) {}

GameHarness::GameHarness(const std::filesystem::path& accountDataDir, game::ClientMover* clientMover) {
    useEmptyExtensionsDir();
    services_.accountDataDir = accountDataDir.string();
    services_.clientMover = clientMover;

    if (expansions_.initialize(sourceRoot() + "Data") == 0 || !expansions_.setActive("wotlk")) {
        fail("no wotlk profile under " + sourceRoot() + "Data/expansions");
    }

    // A table the test has not given reads as one with no rows, wider than
    // any the game reads: a world whose data has nothing in it. The game asks
    // for spell names, skill lines and area triggers on paths a test about
    // something else walks, and finds none, as it would in an empty file.
    auto noRows = std::make_shared<pipeline::DBCFile>();
    if (!noRows->load(wdbcFile(512, {}, std::string(1, '\0')))) fail("an empty table does not load");
    noRows_ = std::move(noRows);
    gameData_ = std::make_unique<game::GameData>(
        [this](const std::string& dbc) {
            dbcsRead_.insert(dbc);
            const auto it = dbcs_.find(dbc);
            return it != dbcs_.end() ? it->second : noRows_;
        },
        expansions_,
        // Where the client logs the line and aborts, a test sees it thrown.
        [](const std::string& line) { throw GameStopped(line); });
    services_.gameData = gameData_.get();
    emotes_ = std::make_unique<game::EmoteTable>(*gameData_);
    services_.expansionRegistry = &expansions_;
    services_.emotes = emotes_.get();
    statTables_ = std::make_unique<game::StatTables>(*gameData_);
    services_.statTables = statTables_.get();
    services_.events = &events_;
    services_.cvars = &cvars_;
    services_.steadyNow = [this] { return now_; };
    services_.makeConnection = [this] {
        // What a live server says without being asked twice: CMSG_PING is
        // answered with SMSG_PONG and the same sequence.
        auto answer = [this](const network::Packet& sent) -> std::optional<network::Packet> {
            if (!pingsAnswered_ || !game_) return std::nullopt;
            if (game_->getOpcodeTable().fromWire(sent.getOpcode()) != game::Opcode::CMSG_PING)
                return std::nullopt;
            network::Packet ping = sent;
            ping.setReadPos(0);
            const uint32_t sequence = ping.readUInt32();
            const uint16_t pong = game_->getOpcodeTable().toWire(game::Opcode::SMSG_PONG);
            return network::Packet(pong, Body().u32(sequence).bytes());
        };
        // A connection the game lets go of is no server to play any more.
        auto made = std::make_shared<FakeWorldConnection*>(nullptr);
        auto connection = std::make_unique<FakeWorldConnection>(answer, connectionsRefused_, [this, made] {
            if (connection_ == *made) connection_ = nullptr;
        });
        *made = connection.get();
        connection_ = connection.get();
        return connection;
    };
    cvars_.load();

    game_ = std::make_unique<game::GameHandler>(services_);
    contactsGateway_ = std::make_unique<game::ContactsGateway>(*game_, *game_);
    contacts_ = std::make_unique<contacts::ContactsService>(*contactsGateway_, *contactsGateway_, events_);
    services_.contactsFeed = contacts_.get();
    whoGateway_ = std::make_unique<game::WhoGateway>(*game_);
    who_ = std::make_unique<who::WhoService>(*whoGateway_, events_);
    services_.whoFeed = who_.get();
    duelGateway_ = std::make_unique<game::DuelGateway>(*game_, *game_);
    duels_ = std::make_unique<duel::DuelService>(*duelGateway_, *duelGateway_, events_);
    services_.duelFeed = duels_.get();
    petitionGateway_ = std::make_unique<game::PetitionGateway>(*game_);
    petitions_ = std::make_unique<petition::PetitionService>(*petitionGateway_, events_);
    services_.petitionFeed = petitions_.get();
    arenaGateway_ = std::make_unique<game::ArenaGateway>(*game_);
    arenaTeams_ = std::make_unique<arena::ArenaService>(*arenaGateway_, events_);
    services_.arenaFeed = arenaTeams_.get();
    battlegroundGateway_ = std::make_unique<game::BattlegroundGateway>(*game_, *game_);
    battlegrounds_ = std::make_unique<battleground::BattlegroundService>(*battlegroundGateway_, *battlegroundGateway_,
                                                                         events_);
    services_.battlegroundFeed = battlegrounds_.get();
    lfgGateway_ = std::make_unique<game::LfgGateway>(*game_, *game_);
    lfg_ = std::make_unique<lfg::LfgService>(*lfgGateway_, *lfgGateway_, events_);
    services_.lfgFeed = lfg_.get();
    calendarGateway_ = std::make_unique<game::CalendarGateway>(*game_);
    calendar_ = std::make_unique<calendar::CalendarService>(*calendarGateway_, events_);
    services_.calendarFeed = calendar_.get();
    groupGateway_ = std::make_unique<game::GroupGateway>(*game_, *game_);
    group_ = std::make_unique<group::GroupService>(*groupGateway_, events_);
    services_.groupFeed = group_.get();
    services_.group = group_.get();
    guildGateway_ = std::make_unique<game::GuildGateway>(*game_, *game_);
    guildNames_ = std::make_unique<MemoryGuildNames>();
    guild_ = std::make_unique<guild::GuildService>(*guildGateway_, *guildGateway_, *guildNames_, events_);
    services_.guildFeed = guild_.get();
    services_.guild = guild_.get();
    auctionGateway_ = std::make_unique<game::AuctionGateway>(*game_, game_->inventory(), *game_);
    auctionHouse_ = std::make_unique<auction::AuctionService>(*auctionGateway_, *auctionGateway_, events_);
    services_.auctionFeed = auctionHouse_.get();
    mailGateway_ = std::make_unique<game::MailGateway>(*game_, game_->inventory(), *game_, game_->queryCache());
    mail_ = std::make_unique<mail::MailService>(*mailGateway_, *mailGateway_, events_);
    services_.mailFeed = mail_.get();
    services_.mail = mail_.get();
    guildBankGateway_ = std::make_unique<game::GuildBankGateway>(*game_, game_->inventory(), *game_);
    guildBank_ = std::make_unique<guild_bank::GuildBankService>(*guildBankGateway_, *guildBankGateway_, events_);
    services_.guildBankFeed = guildBank_.get();
    services_.guildBank = guildBank_.get();
    tradeGateway_ = std::make_unique<game::TradeGateway>(*game_, game_->inventory(), *game_, cvars_);
    trade_ = std::make_unique<trade::TradeService>(*tradeGateway_, *tradeGateway_, events_);
    services_.tradeFeed = trade_.get();
    services_.trade = trade_.get();
    lootGateway_ = std::make_unique<game::LootGateway>(*game_, game_->inventory(), *game_);
    loot_ = std::make_unique<loot::LootService>(*lootGateway_, *lootGateway_, events_);
    services_.lootFeed = loot_.get();
    services_.loot = loot_.get();
    achievements_ = std::make_unique<achievement::AchievementService>(achievementFiles_, events_);
    services_.achievementsFeed = achievements_.get();
    services_.achievements = achievements_.get();
    titleGateway_ = std::make_unique<game::TitleGateway>(*game_, *game_);
    titles_ = std::make_unique<title::TitleService>(titleFiles_, *titleGateway_, events_);
    services_.titlesFeed = titles_.get();
    services_.titles = titles_.get();
    reputationGateway_ = std::make_unique<game::ReputationGateway>(*game_, *game_);
    reputation_ = std::make_unique<reputation::ReputationService>(factionFiles_, *reputationGateway_, events_);
    services_.reputationFeed = reputation_.get();
    services_.reputation = reputation_.get();
    petGateway_ = std::make_unique<game::PetGateway>(*game_);
    pet_ = std::make_unique<pet::PetService>(*petGateway_, events_);
    services_.petFeed = pet_.get();
    services_.pet = pet_.get();
    game::loadProtocolTables(*game_, *gameData_);
    game_->setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
        addonEvents_.push_back({name, args});
    });
}

void GameHarness::useExpansion(const std::string& id) {
    if (!expansions_.setActive(id)) fail("no expansion profile named " + id);
    game::loadProtocolTables(*game_, *gameData_);
}

GameHarness::~GameHarness() = default;

contacts::IContacts& GameHarness::contacts() { return *contacts_; }
who::IWho& GameHarness::who() { return *who_; }
duel::IDuels& GameHarness::duels() { return *duels_; }
petition::IPetitions& GameHarness::petitions() { return *petitions_; }
arena::IArenaTeams& GameHarness::arenaTeams() { return *arenaTeams_; }
battleground::IBattlegrounds& GameHarness::battlegrounds() { return *battlegrounds_; }
lfg::ILfg& GameHarness::lfg() { return *lfg_; }
calendar::ICalendar& GameHarness::calendar() { return *calendar_; }
group::IGroup& GameHarness::group() { return *group_; }
guild::IGuild& GameHarness::guild() { return *guild_; }
auction::IAuctionHouse& GameHarness::auctionHouse() { return *auctionHouse_; }
mail::IMailbox& GameHarness::mail() { return *mail_; }
guild_bank::IGuildBank& GameHarness::guildBank() { return *guildBank_; }
trade::ITrade& GameHarness::trade() { return *trade_; }
loot::ILoot& GameHarness::loot() { return *loot_; }
achievement::IAchievements& GameHarness::achievements() { return *achievements_; }
title::ITitles& GameHarness::titles() { return *titles_; }
reputation::IReputation& GameHarness::reputation() { return *reputation_; }
pet::IPet& GameHarness::pet() { return *pet_; }

FakeWorldConnection& GameHarness::server() {
    if (!connection_) fail("no connection yet: connect (or enterWorld) first");
    return *connection_;
}

void GameHarness::receive(game::LogicalOpcode op, const std::vector<uint8_t>& body) {
    const uint16_t wire = game_->getOpcodeTable().toWire(op);
    if (wire == 0xFFFF) fail("opcode has no wire value in the active table");
    receive(network::Packet(wire, body));
}

void GameHarness::receive(network::Packet packet) {
    server().deliver(std::move(packet));
}

void GameHarness::pump() {
    // A frame delivers what arrived and handles it within a time budget, so a
    // burst can take a few. A thousand is far past any test's burst and short
    // of hanging the run when something never drains.
    for (int frames = 0; frames < 1000; ++frames) {
        frame(1.0f / 60.0f);
        if (game_->queuedIncomingPacketCount() == 0) return;
    }
    fail("packets still queued after 1000 frames");
}

void GameHarness::advance(float seconds) {
    constexpr float kFrame = 1.0f / 60.0f;
    for (float passed = 0.0f; passed < seconds; passed += kFrame) {
        frame(kFrame);
    }
}

void GameHarness::frame(float seconds) {
    now_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
    game_->update(seconds);
    game_->dispatchAddonEvents();
}

std::vector<game::LogicalOpcode> GameHarness::sentOpcodes() const {
    std::vector<game::LogicalOpcode> out;
    if (!connection_) return out;
    for (const auto& packet : connection_->sent()) {
        if (auto op = game_->getOpcodeTable().fromWire(packet.getOpcode())) out.push_back(*op);
    }
    return out;
}

network::Packet* GameHarness::lastSent(game::LogicalOpcode op) {
    if (!connection_) return nullptr;
    auto& sent = connection_->sent();
    for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
        if (game_->getOpcodeTable().fromWire(it->getOpcode()) == op) {
            it->setReadPos(0);
            return &*it;
        }
    }
    return nullptr;
}

size_t GameHarness::countSent(game::LogicalOpcode op) const {
    size_t n = 0;
    for (auto sent : sentOpcodes()) n += (sent == op);
    return n;
}

std::vector<AddonEvent> GameHarness::addonEvents(const std::string& name) const {
    std::vector<AddonEvent> out;
    for (const auto& e : addonEvents_) {
        if (e.name == name) out.push_back(e);
    }
    return out;
}

size_t GameHarness::firedAt(const std::string& name, const std::string& firstArg) const {
    for (size_t i = 0; i < addonEvents_.size(); ++i) {
        const auto& e = addonEvents_[i];
        if (e.name != name) continue;
        if (!firstArg.empty() && (e.args.empty() || e.args[0] != firstArg)) continue;
        return i;
    }
    return addonEvents_.size();
}

void GameHarness::clearRecords() {
    addonEvents_.clear();
    if (connection_) connection_->clearSent();
}

std::vector<uint8_t> GameHarness::authChallengeBody(uint32_t serverSeed) {
    // WotLK: a one, the seed, and 32 bytes of the server's own seeds.
    return Body().u32(1).u32(serverSeed).zeros(32);
}

std::vector<uint8_t> GameHarness::charEnumBody(const PlayerSpec& p) {
    return charEnumBody(std::vector<PlayerSpec>{p});
}

std::vector<uint8_t> GameHarness::charEnumBody(const std::vector<PlayerSpec>& characters) {
    Body b;
    b.u8(static_cast<uint8_t>(characters.size()));
    for (const PlayerSpec& p : characters) {
        b.u64(p.guid).cstr(p.name);
        b.u8(p.race).u8(p.classId).u8(0 /* male */);
        b.u32(0).u8(0);                // skin, face, hair style and colour; facial hair
        b.u8(p.level);
        b.u32(12 /* Elwynn */).u32(p.mapId).f32(p.x).f32(p.y).f32(p.z);
        b.u32(p.guildId);
        b.u32(0);                      // character flags
        b.u32(0).u8(0);                // customization flags, first login
        b.u32(0).u32(0).u32(0);        // pet: display, level, family
        for (int slot = 0; slot < 23; ++slot) {
            b.u32(0).u8(0).u32(0);     // display, inventory type, enchant aura
        }
    }
    return b.bytes();
}

std::vector<uint8_t> GameHarness::loginVerifyWorldBody(const PlayerSpec& p) {
    return Body().u32(p.mapId).f32(p.x).f32(p.y).f32(p.z).f32(0.0f);
}

namespace {

/// One block's fields as the wire carries them: the mask words, then each set
/// field's value in ascending order.
void writeFields(Body& b, const GameHarness::Fields& fields) {
    const int words = fields.empty() ? 0 : fields.rbegin()->first / 32 + 1;
    b.u8(static_cast<uint8_t>(words));
    for (int w = 0; w < words; ++w) {
        uint32_t mask = 0;
        for (const auto& [field, value] : fields) {
            if (field / 32 == w) mask |= 1u << (field % 32);
        }
        b.u32(mask);
    }
    for (const auto& [field, value] : fields) b.u32(value);
}

} // namespace

void GameHarness::putGuid(Fields& fields, uint16_t field, uint64_t guid) {
    fields[field] = static_cast<uint32_t>(guid);
    fields[static_cast<uint16_t>(field + 1)] = static_cast<uint32_t>(guid >> 32);
}

std::vector<uint8_t> GameHarness::createItemBody(uint64_t guid, uint32_t entry, uint32_t stack) {
    Fields fields;
    fields[game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY)] = entry;
    fields[game::fieldIndex(game::UF::ITEM_FIELD_STACK_COUNT)] = stack;
    Body b;
    b.u32(1);                       // one block
    b.u8(2);                        // CREATE_OBJECT
    b.packedGuid(guid);
    b.u8(1);                        // ITEM
    b.u8(0).u8(0);                  // update flags: no movement
    writeFields(b, fields);
    return b.bytes();
}

std::vector<uint8_t> GameHarness::itemTemplateBody(uint32_t entry, const std::string& name,
                                                   uint32_t displayInfoId, uint32_t inventoryType) {
    Body b;
    b.u32(entry);
    b.u32(2).u32(7);                                // Class, SubClass: a one-handed sword
    b.u32(0xFFFFFFFF);                              // SoundOverrideSubclass
    b.cstr(name).u8(0).u8(0).u8(0);                 // Name, Name2..Name4
    b.u32(displayInfoId);
    b.u32(2);                                       // Quality
    b.u32(0).u32(0);                                // Flags, Flags2
    b.u32(10000).u32(2000);                         // BuyPrice, SellPrice
    b.u32(inventoryType);
    b.u32(0xFFFFFFFF).u32(0xFFFFFFFF);              // AllowableClass, AllowableRace
    b.u32(60).u32(55);                              // ItemLevel, RequiredLevel
    for (int i = 0; i < 8; ++i) b.u32(0);           // RequiredSkill .. MaxCount
    b.u32(1).u32(0);                                // Stackable, ContainerSlots
    b.u32(0);                                       // StatsCount
    b.u32(0).u32(0);                                // ScalingStatDistribution, -Value
    for (int i = 0; i < 2; ++i) b.f32(0.0f).f32(0.0f).u32(0);  // damage: min, max, type
    b.u32(0);                                       // Armor
    for (int i = 0; i < 6; ++i) b.u32(0);           // resistances
    b.u32(2600).u32(0).f32(0.0f);                   // Delay, AmmoType, RangedModRange
    for (int s = 0; s < 5; ++s)                     // spells: id, trigger, charges, cooldowns
        b.u32(0).u32(0).u32(0).u32(0xFFFFFFFF).u32(0).u32(0xFFFFFFFF);
    b.u32(1).cstr("");                              // Bonding, Description
    for (int i = 0; i < 10; ++i) b.u32(0);          // PageText .. Block
    b.u32(0).u32(90).u32(0).u32(0xFFFFFFFF);        // ItemSet, MaxDurability, Area, Map
    b.u32(0).u32(0);                                // BagFamily, TotemCategory
    for (int s = 0; s < 3; ++s) b.u32(0).u32(0);    // sockets: colour, content
    b.u32(0).u32(0).u32(0);                         // SocketBonus, GemProperties, DisenchantSkill
    b.f32(0.0f);                                    // ArmorDamageModifier
    b.u32(0).u32(0).u32(0);                         // Duration, ItemLimitCategory, HolidayId
    return b.bytes();
}

std::vector<uint8_t> GameHarness::valuesBody(uint64_t guid, const Fields& fields) {
    Body b;
    b.u32(1);
    b.u8(0);                        // VALUES
    b.packedGuid(guid);
    writeFields(b, fields);
    return b.bytes();
}

void GameHarness::playerInView() {
    // The realm's create block for the character's own object, every field
    // zero and so left out: what a test then fills with playerFields.
    receive(game::Opcode::SMSG_UPDATE_OBJECT,
            createObjectBody(player_.guid, game::ObjectType::PLAYER, {}, player_.x, player_.y, player_.z));
    pump();
}

std::vector<uint8_t> GameHarness::gameObjectTemplateBody(uint32_t entry, uint32_t type,
                                                         const std::string& name,
                                                         const std::array<uint32_t, 24>& data) {
    Body b;
    // display id, the four names, then icon, cast bar caption and unk1.
    b.u32(entry).u32(type).u32(0xD15).cstr(name).cstr("").cstr("").cstr("").cstr("").cstr("").cstr("");
    for (uint32_t word : data) b.u32(word);
    return b.bytes();
}

std::vector<uint8_t> GameHarness::nameResponseBody(uint64_t guid, const std::string& name) {
    return Body().packedGuid(guid).u8(0 /* found */).cstr(name).cstr("").u8(2).u8(0).u8(7);
}

void GameHarness::playerFields(const std::map<uint16_t, uint32_t>& fields, bool create) {
    game::UpdateBlock block{};
    block.guid = player_.guid;
    block.objectType = game::ObjectType::PLAYER;
    block.updateType = create ? game::UpdateType::CREATE_OBJECT : game::UpdateType::VALUES;
    for (const auto& [k, v] : fields) block.fields[k] = v;
    game::UpdateObjectData batch{};
    batch.blocks.push_back(block);
    game_->world().applyRecordedObjects(batch);
    pump();
}

void GameHarness::gameTable(const std::string& dbc, uint32_t column, std::vector<float> rows) {
    std::vector<DbcRow> table;
    table.reserve(rows.size());
    for (float value : rows) table.push_back({{column, value}});
    this->dbc(dbc, column + 1, table);
}

void GameHarness::unreadableDbc(const std::string& dbc) {
    dbcs_[dbc] = nullptr;
}

GameHarness::DbcCell::DbcCell(float v) : number(std::bit_cast<uint32_t>(v)) {}

void GameHarness::dbc(const std::string& dbc, uint32_t fields, const std::vector<DbcRow>& rows) {
    // The string block opens with the empty string, as the client's files do,
    // so offset 0 reads as "".
    std::string strings(1, '\0');
    std::vector<std::vector<uint32_t>> cells;
    for (const DbcRow& row : rows) {
        std::vector<uint32_t> out(fields, 0);
        for (const auto& [column, cell] : row) {
            if (column >= fields) fail(dbc + " given a column " + std::to_string(column) + " past its " +
                                       std::to_string(fields) + " fields");
            if (cell.isText) {
                out[column] = static_cast<uint32_t>(strings.size());
                strings += cell.text;
                strings += '\0';
            } else {
                out[column] = cell.number;
            }
        }
        cells.push_back(std::move(out));
    }
    auto table = std::make_shared<pipeline::DBCFile>();
    if (!table->load(wdbcFile(fields, cells, strings))) fail(dbc + " given rows that do not load");
    dbcs_[dbc] = std::move(table);
}

void GameHarness::enterWorld(const PlayerSpec& player) {
    player_ = player;
    const std::vector<uint8_t> sessionKey(40, 0x5A);
    if (!game_->connect("harness.test", 8085, sessionKey, "HARNESS", 12340, 1)) {
        fail("connect refused");
    }
    answerLogin(player);
    game_->selectCharacter(player.guid);
    answerPlayerLogin(player);
    clearRecords();
}

void GameHarness::answerLogin(const PlayerSpec& player) {
    using game::Opcode;
    using game::WorldState;
    pump();
    if (game_->getState() != WorldState::CONNECTED) fail("did not connect");

    receive(Opcode::SMSG_AUTH_CHALLENGE, authChallengeBody(0xC0FFEE));
    pump();
    if (!lastSent(Opcode::CMSG_AUTH_SESSION)) fail("no CMSG_AUTH_SESSION");

    receive(Opcode::SMSG_AUTH_RESPONSE, Body().u8(0x0C));  // AUTH_OK
    pump();
    if (!lastSent(Opcode::CMSG_CHAR_ENUM)) fail("no CMSG_CHAR_ENUM after AUTH_OK");

    receive(Opcode::SMSG_CHAR_ENUM, charEnumBody(player));
    pump();
    if (game_->getState() != WorldState::CHAR_LIST_RECEIVED) fail("character list not taken");
}

void GameHarness::answerPlayerLogin(const PlayerSpec& player) {
    using game::Opcode;
    player_ = player;
    if (!lastSent(Opcode::CMSG_PLAYER_LOGIN)) fail("no CMSG_PLAYER_LOGIN");
    receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, loginVerifyWorldBody(player));
    pump();
    if (game_->getState() != game::WorldState::IN_WORLD)
        fail("not in the world after LOGIN_VERIFY_WORLD");
}

std::vector<uint8_t> GameHarness::createObjectBody(uint64_t guid, game::ObjectType type,
                                                   std::vector<std::pair<uint16_t, uint32_t>> fields,
                                                   float x, float y, float z, float orientation) {
    for (const auto& [index, value] : fields) {
        (void)value;
        if (index == 0xFFFF) fail("an update field the active table does not have");
    }
    std::sort(fields.begin(), fields.end());
    const int words = fields.empty() ? 1 : fields.back().first / 32 + 1;
    std::vector<uint32_t> mask(static_cast<size_t>(words), 0);
    for (const auto& [index, value] : fields) mask[index / 32] |= 1u << (index % 32);

    const bool living = type == game::ObjectType::UNIT || type == game::ObjectType::PLAYER;
    Body b;
    b.u32(1);                                            // one block
    b.u8(static_cast<uint8_t>(game::UpdateType::CREATE_OBJECT));
    b.packedGuid(guid);
    b.u8(static_cast<uint8_t>(type));
    if (living) {
        b.u8(0x20).u8(0x00);                             // update flags: living
        b.u32(0).u8(0).u8(0);                            // movement flags, flags2
        b.u32(0);                                        // time
        b.f32(x).f32(y).f32(z).f32(orientation);         // position, facing
        b.u32(0);                                        // fall time
        for (float speed : {2.5f, 7.0f, 4.5f, 4.72f, 2.5f, 7.0f, 4.5f, 3.14159f, 3.14159f})
            b.f32(speed);                                // walk .. pitch rate
    } else {
        b.u8(0x00).u8(0x00);                             // update flags: none
    }
    b.u8(static_cast<uint8_t>(words));
    for (uint32_t word : mask) b.u32(word);
    for (const auto& [index, value] : fields) {
        (void)index;
        b.u32(value);
    }
    return b.bytes();
}

void GameHarness::relogAs(const PlayerSpec& next) {
    using game::Opcode;
    const PlayerSpec previous = player_;
    player_ = next;

    game_->social().requestLogout(false);
    receive(Opcode::SMSG_LOGOUT_COMPLETE);
    pump();
    game_->returnToCharacterSelect();
    receive(Opcode::SMSG_CHAR_ENUM, charEnumBody({previous, next}));
    pump();
    game_->selectCharacter(next.guid);
    receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, loginVerifyWorldBody(next));
    pump();
    if (!game_->isInWorld()) fail("the next character is not in the world");

    clearRecords();
}

} // namespace wowee::test
