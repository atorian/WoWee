// The pet and the stable, and the service against a recording gateway and the
// real event bus. Only wowee_pet is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "pet/services/pet_service.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace wowee::pet;

namespace {

PetSpellsRecord wolf(std::vector<PetSpell> spells) {
    PetSpellsRecord r;
    r.guid = 0xF140000000000042ull;
    r.mode = PetMode{.react = 1, .command = 1};
    r.actionBar = std::array<uint32_t, kActionBarSlots>{0x07000002, 0x07000001};
    r.spells = std::move(spells);
    return r;
}

template <typename E>
bool has(const std::vector<PetEvent>& events) {
    for (const auto& e : events)
        if (std::holds_alternative<E>(e)) return true;
    return false;
}

struct RecordingGateway final : IPetGateway {
    bool world = true;
    std::vector<std::string> sent;
    bool online() const override { return world; }
    bool connected() const override { return world; }
    void sendAction(uint64_t, uint32_t action, uint64_t target) override {
        sent.push_back("action " + std::to_string(action) + " " + std::to_string(target));
    }
    void sendAutocast(uint64_t, uint32_t spell, bool on) override {
        sent.push_back("autocast " + std::to_string(spell) + (on ? " on" : " off"));
    }
    void sendRename(uint64_t, const std::string& name) override { sent.push_back("rename " + name); }
    void sendAbandon(uint64_t) override { sent.push_back("abandon"); }
    void sendUnlearnTalents() override { sent.push_back("unlearn talents"); }
    void sendStable(uint64_t master, uint8_t slot) override {
        sent.push_back("stable " + std::to_string(master) + ":" + std::to_string(slot));
    }
    void sendUnstable(uint64_t master, uint32_t petNumber) override {
        sent.push_back("unstable " + std::to_string(master) + ":" + std::to_string(petNumber));
    }
    void sendBuyStableSlot(uint64_t master) override { sent.push_back("buy " + std::to_string(master)); }
    void requestStableList(uint64_t master) override { sent.push_back("list " + std::to_string(master)); }
};

} // namespace

TEST_CASE("A pet's spells carry their autocast bit, and an empty slot is no spell", "[pet]") {
    Pet pet;
    auto events = pet.summoned(wolf({{17253, true}, {0, false}, {2649, false}}));
    CHECK(pet.spells() == std::vector<uint32_t>{17253, 2649});
    CHECK(pet.isAutocast(17253));
    CHECK_FALSE(pet.isAutocast(2649));
    CHECK(pet.actionSlot(0) == 0x07000002);
    REQUIRE(has<PetChanged>(events));
    CHECK(std::get<PetChanged>(events[0]).barChanged);
}

TEST_CASE("A pet list cut short keeps what it did not reach", "[pet]") {
    Pet pet;
    pet.summoned(wolf({{17253, true}}));
    PetSpellsRecord shortList;
    shortList.guid = 0xF140000000000042ull;
    shortList.mode = PetMode{.react = 2, .command = 0};
    pet.summoned(shortList);
    CHECK(pet.mode().react == 2);
    CHECK(pet.spells() == std::vector<uint32_t>{17253});
    CHECK(pet.actionSlot(1) == 0x07000001);
}

TEST_CASE("A dismissed pet takes its bar and spells with it", "[pet]") {
    Pet pet;
    pet.summoned(wolf({{17253, true}}));
    auto events = pet.dismissed();
    CHECK(pet.guid() == 0);
    CHECK(pet.spells().empty());
    CHECK_FALSE(pet.isAutocast(17253));
    CHECK(pet.actionSlot(0) == 0);
    REQUIRE(has<PetChanged>(events));
    CHECK_FALSE(std::get<PetChanged>(events[0]).barChanged);
}

TEST_CASE("Only crossing into or out of attack is announced", "[pet]") {
    Pet pet;
    pet.summoned(wolf({}));
    const uint64_t guid = pet.guid();
    CHECK(has<PetAttackStarted>(pet.modeChanged(guid, {.react = 1, .command = 2})));
    CHECK(pet.modeChanged(guid, {.react = 2, .command = 2}).empty());
    CHECK(has<PetAttackStopped>(pet.modeChanged(guid, {.react = 2, .command = 1})));
    CHECK(pet.modeChanged(guid + 1, {.react = 0, .command = 2}).empty());
    CHECK(pet.mode().command == 1);
}

TEST_CASE("Experience is announced when it moves, stats whenever they arrive", "[pet]") {
    Pet pet;
    PetFields xp;
    xp.experience = 500;
    CHECK(has<PetExperienceChanged>(pet.fieldsArrived(xp)));
    CHECK(pet.fieldsArrived(xp).empty());

    PetFields stats;
    stats.stats[0] = 120;
    stats.resistances[0] = 3000;
    stats.minDamage = 41.5f;
    auto events = pet.fieldsArrived(stats);
    CHECK(has<PetStatsChanged>(events));
    CHECK(pet.stats()[0] == 120);
    CHECK(pet.resistances()[0] == 3000);
    CHECK(pet.minDamage() == 41.5f);
    CHECK(pet.experience() == 500);
}

TEST_CASE("Unlearning a spell turns its autocast off too", "[pet]") {
    Pet pet;
    pet.summoned(wolf({{17253, true}, {2649, true}}));
    CHECK(has<PetUnlearnedSpell>(pet.unlearned(17253)));
    CHECK(pet.spells() == std::vector<uint32_t>{2649});
    CHECK_FALSE(pet.isAutocast(17253));
}

TEST_CASE("The stable announces closing only if it was open", "[pet]") {
    Stable stable;
    CHECK(stable.close().empty());
    auto shown = stable.listed(77, 2, {});
    CHECK(has<StableShown>(shown));
    CHECK(has<StableClosed>(stable.close()));
    CHECK_FALSE(stable.isOpen());
}

TEST_CASE("Dismissing sends the abandon command, not stay", "[pet]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetService service{gateway, bus};
    service.dismissPet();
    service.onPetSpells(wolf({}));
    service.dismissPet();
    CHECK(gateway.sent == std::vector<std::string>{"action " + std::to_string(0x07000003u) + " 0"});
}

TEST_CASE("Toggling autocast sends the new state and remembers it", "[pet]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetService service{gateway, bus};
    service.onPetSpells(wolf({{17253, true}}));
    service.togglePetSpellAutocast(17253);
    service.togglePetSpellAutocast(17253);
    gateway.world = false;
    service.togglePetSpellAutocast(17253);
    CHECK(gateway.sent == std::vector<std::string>{"autocast 17253 off", "autocast 17253 on"});
    CHECK(service.isPetSpellAutocast(17253));
}

TEST_CASE("A pet's name is twelve letters at most", "[pet]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetService service{gateway, bus};
    service.onPetSpells(wolf({}));
    service.renamePet("Thirteenchars");
    service.renamePet("");
    service.renamePet("Twelvechars!");
    CHECK(gateway.sent == std::vector<std::string>{"rename Twelvechars!"});
}

TEST_CASE("A stable master's list is asked for, and refreshed after a change while open", "[pet]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetService service{gateway, bus};
    std::vector<uint8_t> results;
    auto sub = bus.subscribe<StableResult>([&](const StableResult& e) { results.push_back(e.result); });
    service.buyStableSlot();                // no stable master yet: nothing to ask
    service.setStableMaster(77);
    service.requestStabledPetList();
    service.onStableResult(0x01);           // window not open yet: no refresh
    service.onStableList(77, 2, {});
    service.onStableResult(0x07);           // slot bought: refresh
    service.onStableResult(0x09);           // failed: nothing changed
    service.buyStableSlot();
    CHECK(gateway.sent == std::vector<std::string>{"list 77", "list 77", "buy 77"});
    CHECK(results == std::vector<uint8_t>{0x01, 0x07, 0x09});
}

TEST_CASE("A pet is stabled only when one is out, and the player is told when none is", "[pet]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetService service{gateway, bus};
    int refused = 0;
    auto sub = bus.subscribe<NoPetToStable>([&](const auto&) { ++refused; });
    service.setStableMaster(77);
    service.stablePet(2);
    CHECK(refused == 1);
    service.onPetSpells(wolf({}));
    service.stablePet(2);
    service.unstablePet(0);
    service.unstablePet(5);
    CHECK(gateway.sent == std::vector<std::string>{"stable 77:2", "unstable 77:5"});
    CHECK(refused == 1);
}

TEST_CASE("A pet talent reset is asked for once the player accepts the offer", "[pet]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetService service{gateway, bus};
    int requested = 0;
    auto sub = bus.subscribe<PetTalentResetRequested>([&](const auto&) { ++requested; });
    service.confirmPetUnlearn();            // nothing was offered
    service.onUnlearnOffered(5000);
    CHECK(service.showPetUnlearnDialog());
    CHECK(service.getPetUnlearnCost() == 5000);
    service.confirmPetUnlearn();
    service.confirmPetUnlearn();
    CHECK_FALSE(service.showPetUnlearnDialog());
    CHECK(gateway.sent == std::vector<std::string>{"unlearn talents"});
    CHECK(requested == 1);

    service.onUnlearnOffered(5000);
    service.cancelPetUnlearn();
    CHECK_FALSE(service.showPetUnlearnDialog());
    CHECK(gateway.sent.size() == 1);
}

TEST_CASE("The next character has none of the last one's pet, stable or offer", "[pet]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetService service{gateway, bus};
    service.onPetSpells(wolf({{17253, true}}));
    service.onRenameable();
    service.onUnlearnOffered(5000);
    service.onStableList(77, 2, {StabledPet{}});
    service.beginCharacter();
    CHECK_FALSE(service.hasPet());
    CHECK(service.getPetSpells().empty());
    CHECK(service.getPetActionSlot(0) == 0);
    CHECK_FALSE(service.isPetRenameable());
    CHECK_FALSE(service.showPetUnlearnDialog());
    CHECK_FALSE(service.isStableWindowOpen());
    CHECK(service.getStabledPets().empty());
    CHECK(service.getStableSlots() == 0);
    service.buyStableSlot();
    CHECK(gateway.sent.empty());
}
