// The pet, end to end: the server summoning one and saying what it knows, the
// player commanding it, its name and stats, the stable, and what the
// interface is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/entity.hpp"
#include "pet/domain/records.hpp"
#include "pet/services/pet.hpp"
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

/// The realm puts the wolf in view: SMSG_UPDATE_OBJECT creating a unit of its
/// creature entry, a timber wolf.
void wolfInView(GameHarness& h) {
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kWolf, wowee::game::ObjectType::UNIT,
                                            {{wowee::game::fieldIndex(wowee::game::UF::OBJECT_FIELD_ENTRY), 69},
                                             {wowee::game::fieldIndex(wowee::game::UF::UNIT_FIELD_HEALTH), 50}}));
    h.pump();
}

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
    for (int i = 1; i < wowee::pet::kActionBarSlots; ++i) b.u32(0);
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
    auto& pet = h.pet();

    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 2, 1, {{kBite, 0xC1}, {kGrowl, 0x81}}));
    h.pump();

    REQUIRE(pet.hasPet());
    CHECK(pet.getPetGuid() == kWolf);
    CHECK(pet.getPetReact() == 2);
    CHECK(pet.getPetCommand() == 1);
    CHECK((pet.getPetActionSlot(0) & 0x00FFFFFFu) == kBite);
    CHECK(pet.getPetSpells() == std::vector<uint32_t>{kBite, kGrowl});
    CHECK(pet.isPetSpellAutocast(kBite));
    CHECK_FALSE(pet.isPetSpellAutocast(kGrowl));
    CHECK(h.addonEvents("UNIT_PET").size() == 1);
    CHECK(h.addonEvents("PET_BAR_UPDATE").size() == 1);
    // What it is called is asked for: nothing else brings the name a player
    // gave a pet.
    auto* query = h.lastSent(Opcode::CMSG_PET_NAME_QUERY);
    REQUIRE(query);
    const uint32_t key = query->readUInt32();
    CHECK(query->readUInt64() == kWolf);

    // The answer names the pet in view.
    wolfInView(h);
    h.receive(Opcode::SMSG_PET_NAME_QUERY_RESPONSE, Body().u32(key).cstr("Fang").u32(0));
    h.pump();
    auto wolf = std::dynamic_pointer_cast<wowee::game::Unit>(h.game().getEntityManager().getEntity(kWolf));
    REQUIRE(wolf);
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
    CHECK(h.pet().getPetCommand() == 2);
    CHECK(h.pet().getPetReact() == 2);
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
    const auto& pet = h.pet();
    CHECK_FALSE(pet.hasPet());
    CHECK(pet.getPetSpells().empty());
    CHECK(pet.getPetActionSlot(0) == 0);
    CHECK(h.addonEvents("UNIT_PET").size() == 1);
    CHECK(h.addonEvents("PET_UI_UPDATE").size() == 1);
}

TEST_CASE("the pet's own fields are the pet's stats, not the player's", "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {}));
    h.pump();
    wolfInView(h);

    GameHarness::Fields fields;
    fields[wowee::game::fieldIndex(wowee::game::UF::UNIT_FIELD_STAT0)] = 77;
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(kWolf, fields));
    h.pump();
    CHECK(h.pet().getPetStats()[0] == 77);
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

    const auto& pet = h.pet();
    REQUIRE(pet.getStabledPets().size() == 2);
    CHECK(pet.getStabledPets()[0].name == "Fang");
    CHECK(pet.getStabledPets()[0].isActive);
    CHECK(pet.getStabledPets()[1].name == "Hoot");
    CHECK_FALSE(pet.getStabledPets()[1].isActive);
    CHECK(pet.getStableSlots() == 2);
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
    REQUIRE(h.pet().hasPet());

    h.relogAs(mage);

    const auto& pet = h.pet();
    CHECK_FALSE(pet.hasPet());
    // The wolf's bar and spells were left behind with the guid cleared, and a
    // mage's pet bar read the hunter's Bite.
    CHECK(pet.getPetSpells().empty());
    CHECK(pet.getPetActionSlot(0) == 0);
    CHECK_FALSE(pet.isPetSpellAutocast(kBite));
}

TEST_CASE("a spell the pet learns is said and redraws the bar, and one it unlearns leaves it",
          "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {{kBite, 0xC1}}));
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_PET_LEARNED_SPELL, Body().u32(kGrowl).bytes());
    h.pump();
    CHECK(h.pet().getPetSpells() == std::vector<uint32_t>{kBite, kGrowl});
    CHECK(h.addonEvents("PET_BAR_UPDATE").size() == 1);
    const auto& history = h.game().chat().getChatHistory();
    REQUIRE_FALSE(history.empty());
    CHECK(history.back().message.rfind("Your pet has learned ", 0) == 0);

    h.receive(Opcode::SMSG_PET_UNLEARNED_SPELL, Body().u32(kBite).bytes());
    h.pump();
    CHECK(h.pet().getPetSpells() == std::vector<uint32_t>{kGrowl});
    CHECK_FALSE(h.pet().isPetSpellAutocast(kBite));
    CHECK(h.addonEvents("PET_BAR_UPDATE").size() == 2);
}

TEST_CASE("the pet's experience bar is told when the experience moves, and only then", "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {}));
    h.pump();
    wolfInView(h);
    h.clearRecords();

    GameHarness::Fields xp{{wowee::game::fieldIndex(wowee::game::UF::UNIT_FIELD_PETEXPERIENCE), 500}};
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(kWolf, xp));
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(kWolf, xp));
    h.pump();

    CHECK(h.pet().getPetExperience() == 500);
    const auto told = h.addonEvents("UNIT_PET_EXPERIENCE");
    REQUIRE(told.size() == 1);
    CHECK(told[0].args == std::vector<std::string>{"pet"});
}

TEST_CASE("a pet talent reset waits for the player's yes, and the line says what was asked",
          "[pet][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PET_SPELLS, petSpells(kWolf, 1, 1, {}));
    h.pump();

    // SMSG_PET_UNLEARN_CONFIRM: the pet, then the cost in copper.
    h.receive(Opcode::SMSG_PET_UNLEARN_CONFIRM, Body().u64(kWolf).u32(5000).bytes());
    h.pump();
    REQUIRE(h.pet().showPetUnlearnDialog());
    CHECK(h.pet().getPetUnlearnCost() == 5000);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_PET_UNLEARN_TALENTS));

    h.pet().confirmPetUnlearn();
    h.pump();
    CHECK(h.lastSent(Opcode::CMSG_PET_UNLEARN_TALENTS));
    CHECK_FALSE(h.pet().showPetUnlearnDialog());
    const auto& history = h.game().chat().getChatHistory();
    REQUIRE_FALSE(history.empty());
    CHECK(history.back().message.rfind("Pet talent reset requested.", 0) == 0);
}

TEST_CASE("stabling with no pet out is refused on the error line, and nothing is sent",
          "[pet][stable][harness]") {
    GameHarness h;
    h.enterWorld();
    constexpr uint64_t kStableMaster = 0xF130000000000201ull;
    // The stable master's list: no pets in it, two slots bought.
    h.receive(Opcode::MSG_LIST_STABLED_PETS, Body().u64(kStableMaster).u8(0).u8(2));
    h.pump();
    h.clearRecords();

    h.pet().stablePet(1);
    h.pump();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_STABLE_PET));
    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args.at(0) == "You do not have an active pet to stable.");
}
