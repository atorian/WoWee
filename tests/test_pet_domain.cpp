// The pet, end to end: the server summoning one and saying what it knows, the
// player commanding it, its name and stats, the stable, and what the
// interface is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/entity.hpp"
#include "game/pet_handler.hpp"
#include "game/update_field_table.hpp"

#include <memory>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kWolf = 0xF140000000000101ull;
constexpr uint32_t kBite = 17253;
constexpr uint32_t kGrowl = 2649;

/// SMSG_PET_SPELLS as AzerothCore's Player::PetSpellInitialize writes it on
/// 3.3.5a: the pet, its family, duration, react and command, flags, the ten
/// bar slots, and the spells with their state in the top byte.
std::vector<uint8_t> petSpells(uint64_t pet, uint8_t react, uint8_t command,
                               const std::vector<std::pair<uint32_t, uint8_t>>& spells) {
    Body b;
    b.u64(pet).u8(1).u8(0);                 // family 1 (wolf), as a u16
    b.u32(0);                               // permanent
    b.u8(react).u8(command).u8(0).u8(0);    // react, command, flags
    b.u32(kBite | 0xC1000000u);             // slot 1: Bite, autocast on
    for (int i = 1; i < wowee::game::PetHandler::kActionBarSlots; ++i) b.u32(0);
    b.u8(static_cast<uint8_t>(spells.size()));
    for (const auto& [spell, state] : spells) b.u32(spell | (uint32_t(state) << 24));
    return b.bytes();
}

std::vector<uint8_t> petMode(uint64_t pet, uint8_t command, uint8_t react) {
    return Body().u64(pet).u32(uint32_t(command) | (uint32_t(react) << 8)).bytes();
}

}  // namespace

TEST_CASE("a summoned pet's bar and spells are what the interface reads", "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    auto& pet = h.game().pet();

    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 2, 1, {{kBite, 0xC1}, {kGrowl, 0x81}}));
    h.pump();

    REQUIRE(pet.hasPet());
    CHECK(pet.guid() == kWolf);
    CHECK(pet.react() == 2);
    CHECK(pet.command() == 1);
    CHECK((pet.actionSlot(0) & 0x00FFFFFFu) == kBite);
    CHECK(pet.spells() == std::vector<uint32_t>{kBite, kGrowl});
    CHECK(pet.isAutocast(kBite));
    CHECK_FALSE(pet.isAutocast(kGrowl));
    CHECK(h.addonEvents("UNIT_PET").size() == 1);
    CHECK(h.addonEvents("PET_BAR_UPDATE").size() == 1);
    // What it is called is asked for: nothing else brings the name a player
    // gave a pet.
    auto* query = h.lastSent(Opcode::CMSG_PET_NAME_QUERY);
    REQUIRE(query);
    const uint32_t key = query->readUInt32();
    CHECK(query->readUInt64() == kWolf);

    // The answer names the pet in view.
    auto wolf = std::make_shared<wowee::game::Unit>(kWolf);
    h.game().getEntityManager().addEntity(kWolf, wolf);
    h.receive(Opcode::SMSG_PET_NAME_QUERY_RESPONSE, Body().u32(key).cstr("Fang").u32(0));
    h.pump();
    CHECK(wolf->getName() == "Fang");
    REQUIRE(h.addonEvents("UNIT_NAME_UPDATE").size() == 1);
    CHECK(h.addonEvents("UNIT_NAME_UPDATE")[0].args == std::vector<std::string>{"pet"});
}

TEST_CASE("a pet sent to attack lights the attack indicator once, and puts it out",
          "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {}));
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_PET_MODE, petMode(kWolf, 2, 1));
    h.receive(Opcode::SMSG_PET_MODE, petMode(kWolf, 2, 2));
    h.pump();
    CHECK(h.game().pet().command() == 2);
    CHECK(h.game().pet().react() == 2);
    CHECK(h.addonEvents("PET_ATTACK_START").size() == 1);

    h.receive(Opcode::SMSG_PET_MODE, petMode(kWolf, 1, 2));
    h.pump();
    CHECK(h.addonEvents("PET_ATTACK_STOP").size() == 1);
}

TEST_CASE("a pet that dies takes its bar and spells with it", "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {{kBite, 0xC1}}));
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_PET_BROKEN);
    h.pump();
    const auto& pet = h.game().pet();
    CHECK_FALSE(pet.hasPet());
    CHECK(pet.spells().empty());
    CHECK(pet.actionSlot(0) == 0);
    CHECK(h.addonEvents("UNIT_PET").size() == 1);
    CHECK(h.addonEvents("PET_UI_UPDATE").size() == 1);
}

TEST_CASE("the pet's own fields are the pet's stats, not the player's", "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {}));
    h.pump();
    h.game().getEntityManager().addEntity(kWolf, std::make_shared<wowee::game::Unit>(kWolf));

    GameHarness::Fields fields;
    fields[wowee::game::fieldIndex(wowee::game::UF::UNIT_FIELD_STAT0)] = 77;
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(kWolf, fields));
    h.pump();
    CHECK(h.game().pet().stats()[0] == 77);
}

TEST_CASE("the stable lists the pets kept there and refreshes after each change",
          "[pet][stable][harness]") {
    GameHarness h;
    h.enterWorld();
    constexpr uint64_t kStableMaster = 0xF130000000000201ull;

    Body list;
    list.u64(kStableMaster).u8(2).u8(2);                       // two pets, two slots
    list.u32(1).u32(299).u32(20).cstr("Fang").u8(1);           // out
    list.u32(2).u32(1201).u32(18).cstr("Hoot").u8(2);          // stabled
    h.receive(Opcode::MSG_LIST_STABLED_PETS, list);
    h.pump();

    const auto& pet = h.game().pet();
    REQUIRE(pet.stabledPets().size() == 2);
    CHECK(pet.stabledPets()[0].name == "Fang");
    CHECK(pet.stabledPets()[0].isActive);
    CHECK(pet.stabledPets()[1].name == "Hoot");
    CHECK_FALSE(pet.stabledPets()[1].isActive);
    CHECK(pet.stableSlots() == 2);
    CHECK(pet.isStableWindowOpen());
    CHECK(h.addonEvents("PET_STABLE_SHOW").size() == 1);

    // A pet stored: the list is asked for again, from the same stable master.
    h.clearRecords();
    h.receive(Opcode::SMSG_STABLE_RESULT, Body().u8(0x01));
    h.pump();
    auto* refresh = h.lastSent(Opcode::MSG_LIST_STABLED_PETS);
    REQUIRE(refresh);
    CHECK(refresh->readUInt64() == kStableMaster);
}

TEST_CASE("the next character has none of the last one's pet", "[pet][harness]") {
    PlayerSpec hunter;
    hunter.guid = 0x31;
    hunter.name = "Huntress";
    PlayerSpec mage;
    mage.guid = 0x32;
    mage.name = "Mage";

    GameHarness h;
    h.enterWorld(hunter);
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {{kBite, 0xC1}}));
    h.pump();
    REQUIRE(h.game().pet().hasPet());

    h.relogAs(mage);

    const auto& pet = h.game().pet();
    CHECK_FALSE(pet.hasPet());
    // The wolf's bar and spells were left behind with the guid cleared, and a
    // mage's pet bar read the hunter's Bite.
    CHECK(pet.spells().empty());
    CHECK(pet.actionSlot(0) == 0);
    CHECK_FALSE(pet.isAutocast(kBite));
}
