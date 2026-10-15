// The account data domain end to end: what the server keeps for the account -
// key bindings, macros - arriving, reaching the action bar and the interface,
// changes going back, and what one session leaves on disk for the next.

#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "test_support.hpp"
#include "game/account_data.hpp"
#include "game/account_data_handler.hpp"
#include "game/game_events.hpp"

#include <zlib.h>

using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::game::AccountDataType;
using wowee::game::ActionBarSlot;
using wowee::game::Opcode;

namespace {

constexpr uint32_t kServerTime = 1760000000;

/// SMSG_ACCOUNT_DATA_TIMES: a time for each slot in the mask, in slot order.
std::vector<uint8_t> times(uint32_t mask, const std::vector<uint32_t>& slotTimes) {
    Body b;
    b.u32(kServerTime).u8(1).u32(mask);
    for (uint32_t t : slotTimes) b.u32(t);
    return b.bytes();
}

/// SMSG_UPDATE_ACCOUNT_DATA carrying this text, compressed as the server does.
std::vector<uint8_t> accountData(uint64_t guid, AccountDataType type, uint32_t time,
                                 const std::string& text) {
    Body b;
    b.u64(guid).u32(static_cast<uint32_t>(type)).u32(time).u32(static_cast<uint32_t>(text.size()));
    std::vector<uint8_t> bytes = b.bytes();
    uLongf len = compressBound(static_cast<uLong>(text.size()));
    std::vector<uint8_t> z(len);
    REQUIRE(compress(z.data(), &len, reinterpret_cast<const Bytef*>(text.data()),
                     static_cast<uLong>(text.size())) == Z_OK);
    bytes.insert(bytes.end(), z.begin(), z.begin() + static_cast<long>(len));
    return bytes;
}

/// SMSG_ACTION_BUTTONS (WotLK): a mode and 144 packed buttons.
std::vector<uint8_t> actionButtons(const std::map<int, uint32_t>& buttons, uint8_t mode = 1) {
    Body b;
    b.u8(mode);
    for (int i = 0; i < 144; ++i) {
        const auto it = buttons.find(i);
        b.u32(it == buttons.end() ? 0u : it->second);
    }
    return b.bytes();
}

/// The slots the game asked the server for, in order.
std::vector<uint32_t> requestedSlots(GameHarness& h) {
    std::vector<uint32_t> out;
    for (auto& packet : h.server().sent()) {
        if (h.game().getOpcodeTable().fromWire(packet.getOpcode()) != Opcode::CMSG_REQUEST_ACCOUNT_DATA) continue;
        packet.setReadPos(0);
        out.push_back(packet.readUInt32());
    }
    return out;
}

struct Upload {
    uint32_t type = 0;
    uint32_t time = 0;
    std::string text;
};

/// The last CMSG_UPDATE_ACCOUNT_DATA, inflated.
Upload lastUpload(GameHarness& h) {
    auto* packet = h.lastSent(Opcode::CMSG_UPDATE_ACCOUNT_DATA);
    REQUIRE(packet);
    Upload up;
    up.type = packet->readUInt32();
    up.time = packet->readUInt32();
    const uint32_t size = packet->readUInt32();
    if (size == 0) return up;
    up.text.resize(size);
    uLongf len = size;
    const auto& raw = packet->getData();
    REQUIRE(uncompress(reinterpret_cast<Bytef*>(up.text.data()), &len,
                       raw.data() + packet->getReadPos(),
                       static_cast<uLong>(raw.size() - packet->getReadPos())) == Z_OK);
    up.text.resize(len);
    return up;
}

} // namespace

TEST_CASE("the account's times are read by their mask and only the newer slots asked for",
          "[accountdata][harness]") {
    GameHarness h;
    h.enterWorld();

    // The account's mask, 0x15: the three times are slots 0, 2 and 4 - and
    // only the key bindings changed on the server.
    h.receive(Opcode::SMSG_ACCOUNT_DATA_TIMES, times(0x15, {0, 500, 0}));
    h.pump();

    CHECK(requestedSlots(h) == std::vector<uint32_t>{static_cast<uint32_t>(AccountDataType::GlobalBindings)});

    // Announced again - the server sends them unasked and again when asked -
    // and still asked for once.
    h.receive(Opcode::SMSG_ACCOUNT_DATA_TIMES, times(0x15, {0, 500, 0}));
    h.pump();
    CHECK(requestedSlots(h).size() == 1);
}

TEST_CASE("bindings the server keeps reach the client as the text the other client saved",
          "[accountdata][harness]") {
    GameHarness h;
    std::vector<std::string> told;
    auto sub = h.events().subscribe<wowee::game::events::AccountDataLoaded>(
        [&](const wowee::game::events::AccountDataLoaded& e) {
            if (e.type == static_cast<uint8_t>(AccountDataType::GlobalBindings)) told.push_back(e.text);
        });
    h.enterWorld();
    told.clear();

    const std::string saved = "BINDINGMODE 0\r\nbind Q MULTIACTIONBAR1BUTTON1\r\nbind 6 NONE\r\n";
    h.receive(Opcode::SMSG_ACCOUNT_DATA_TIMES, times(0x15, {0, 700, 0}));
    h.receive(Opcode::SMSG_UPDATE_ACCOUNT_DATA, accountData(0, AccountDataType::GlobalBindings, 700, saved));
    h.pump();

    REQUIRE(told.size() == 1);
    CHECK(told[0] == saved);
    CHECK(h.game().accountData().text(AccountDataType::GlobalBindings) == saved);
}

TEST_CASE("a character's macro from the server is what its action button holds",
          "[accountdata][harness]") {
    GameHarness h;
    h.enterWorld();

    // The character's mask, 0xEA: slots 1, 3, 5, 6 and 7. Its macros changed.
    h.receive(Opcode::SMSG_ACCOUNT_DATA_TIMES, times(0xEA, {0, 0, 900, 0, 0}));
    h.pump();
    CHECK(requestedSlots(h) == std::vector<uint32_t>{static_cast<uint32_t>(AccountDataType::CharacterMacros)});

    h.receive(Opcode::SMSG_UPDATE_ACCOUNT_DATA,
              accountData(h.player().guid, AccountDataType::CharacterMacros, 900,
                          "MACRO 16777217 \"up\" INV_Misc_QuestionMark\r\n/cast Bloodrage\r\nEND\r\n"));
    // Button 3 holds the character's macro 1: type 0x41, the id's low bits.
    h.receive(Opcode::SMSG_ACTION_BUTTONS, actionButtons({{2, 0x41000001u}}));
    h.pump();

    const auto& slot = h.game().getActionBar()[2];
    REQUIRE(slot.type == ActionBarSlot::MACRO);
    const auto* macro = h.game().accountData().macro(slot.id);
    REQUIRE(macro);
    CHECK(macro->name == "up");
    CHECK(macro->body == "/cast Bloodrage");
    CHECK_FALSE(h.addonEvents("UPDATE_MACROS").empty());
}

TEST_CASE("the server's action bar replaces the one before it, empty buttons included",
          "[accountdata][harness]") {
    GameHarness h;
    h.enterWorld();

    // Nothing on the bar the server did not put there.
    for (const auto& slot : h.game().getActionBar()) CHECK(slot.isEmpty());

    h.receive(Opcode::SMSG_ACTION_BUTTONS, actionButtons({{0, 6603}, {11, 8690}}));
    h.pump();
    CHECK(h.game().getActionBar()[0].id == 6603);

    // The player cleared button 1 and moved the hearthstone, in another client.
    h.receive(Opcode::SMSG_ACTION_BUTTONS, actionButtons({{5, 8690}}));
    h.pump();
    CHECK(h.game().getActionBar()[0].isEmpty());
    CHECK(h.game().getActionBar()[11].isEmpty());
    CHECK(h.game().getActionBar()[5].id == 8690);

    // Mode 2 is the spec switch clearing the bar before the next one arrives.
    h.receive(Opcode::SMSG_ACTION_BUTTONS, Body().u8(2));
    h.pump();
    CHECK(h.game().getActionBar()[5].isEmpty());
}

TEST_CASE("a macro made here goes to the server as the original client writes it",
          "[accountdata][harness]") {
    GameHarness h;
    h.enterWorld();
    auto& data = h.game().accountData();

    const uint32_t heal = data.createMacro(false, "Heal", "Interface\\Icons\\Spell_Holy_Heal", "/cast Heal");
    REQUIRE(heal != 0);
    const Upload account = lastUpload(h);
    CHECK(account.type == static_cast<uint32_t>(AccountDataType::GlobalMacros));
    CHECK(account.time != 0);
    CHECK(account.text == "MACRO " + std::to_string(heal) + " \"Heal\" Spell_Holy_Heal\r\n/cast Heal\r\nEND\r\n");

    // The character's own, on a button: the server is told type 0x41.
    const uint32_t mine = data.createMacro(true, "Charge", "", "/cast Charge\n/startattack");
    REQUIRE(mine != 0);
    CHECK(lastUpload(h).type == static_cast<uint32_t>(AccountDataType::CharacterMacros));
    h.game().setActionBarSlot(0, ActionBarSlot::MACRO, mine);
    auto* set = h.lastSent(Opcode::CMSG_SET_ACTION_BUTTON);
    REQUIRE(set);
    CHECK(set->readUInt8() == 0);
    CHECK(set->readUInt32() == (0x41000000u | (mine & 0x00FFFFFFu)));

    // Deleted, it leaves the account data and the button.
    REQUIRE(data.deleteMacro(mine));
    CHECK(lastUpload(h).text.empty());
    CHECK(h.game().getActionBar()[0].isEmpty());
}

TEST_CASE("account data one session kept is not fetched again, and a change made away is sent",
          "[accountdata][harness]") {
    const auto dir = wowee::test::scratchDir("wowee_account_data_sessions");
    const std::string bindings = "BINDINGMODE 0\r\nbind E MULTIACTIONBAR1BUTTON2\r\n";
    {
        GameHarness h(dir);
        h.enterWorld();
        h.receive(Opcode::SMSG_ACCOUNT_DATA_TIMES, times(0x15, {0, 500, 0}));
        h.receive(Opcode::SMSG_UPDATE_ACCOUNT_DATA, accountData(0, AccountDataType::GlobalBindings, 500, bindings));
        h.pump();
    }
    {
        GameHarness h(dir);
        h.enterWorld();
        // In force from the disk before the server says anything.
        CHECK(h.game().accountData().text(AccountDataType::GlobalBindings) == bindings);
        h.receive(Opcode::SMSG_ACCOUNT_DATA_TIMES, times(0x15, {0, 500, 0}));
        h.pump();
        CHECK(requestedSlots(h).empty());
        CHECK(h.countSent(Opcode::CMSG_UPDATE_ACCOUNT_DATA) == 0);

        // Changed while the server could not be told: the next session finds
        // the copy here newer and sends it.
        h.game().disconnect();
        h.game().accountData().createMacro(false, "Mount", "", "/cast Swift Gryphon");
    }
    {
        GameHarness h(dir);
        h.enterWorld();
        h.receive(Opcode::SMSG_ACCOUNT_DATA_TIMES, times(0x15, {0, 500, 0}));
        h.pump();
        const Upload up = lastUpload(h);
        CHECK(up.type == static_cast<uint32_t>(AccountDataType::GlobalMacros));
        CHECK(up.text.find("\"Mount\"") != std::string::npos);
    }
}

TEST_CASE("bindings-cache.wtf reads as the keys and commands it names, and is written back in its own form",
          "[accountdata]") {
    // As the original client saves it: CRLF, a mode line, commands that hold
    // spaces, and NONE for a default key taken away.
    const std::string text =
        "BINDINGMODE 0\r\n"
        "bind SHIFT-MOUSEWHEELUP MULTIACTIONBAR2BUTTON3\r\n"
        "bind 6 NONE\r\n"
        "bind CTRL-F SPELL Frost Nova\r\n";
    const auto lines = wowee::game::parseBindingsCache(text);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].key == "SHIFT-MOUSEWHEELUP");
    CHECK(lines[0].command == "MULTIACTIONBAR2BUTTON3");
    CHECK(lines[1].key == "6");
    CHECK(lines[1].command == wowee::game::kUnboundCommand);
    CHECK(lines[2].key == "CTRL-F");
    CHECK(lines[2].command == "SPELL Frost Nova");

    CHECK(wowee::game::writeBindingsCache(lines) == text);
}
