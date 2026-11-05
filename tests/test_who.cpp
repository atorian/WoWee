// /who: where an answer goes and what is kept of it, and the service against a
// recording gateway and the real event bus. Only wowee_who is linked.
#include <catch_amalgamated.hpp>

#include "foundation/event_bus.hpp"
#include "who/services/who_service.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace wowee::who;

namespace {

WhoEntry entry(const std::string& name) {
    WhoEntry e;
    e.name = name;
    e.level = 80;
    return e;
}

template <typename E>
bool has(const std::vector<WhoChange>& changes) {
    for (const auto& c : changes)
        if (std::holds_alternative<E>(c)) return true;
    return false;
}

struct RecordingGateway final : IWhoGateway {
    bool world = true;
    std::vector<std::string> sent;
    bool online() const override { return world; }
    void sendQuery(const std::string& name) override { sent.push_back("who " + name); }
};

} // namespace

TEST_CASE("An answer prints to chat unless a panel shows it", "[who]") {
    WhoList w;
    auto chat = w.answered(2, 40, {entry("Ann"), entry("Bob")});
    REQUIRE(has<WhoPrinted>(chat));
    CHECK(std::get<WhoPrinted>(chat.front()).onlineCount == 40);
    CHECK(has<WhoListUpdated>(chat));
    w.showInPanel(true);
    auto panel = w.answered(1, 41, {entry("Ann")});
    CHECK_FALSE(has<WhoPrinted>(panel));
    CHECK(has<WhoListUpdated>(panel));
}

TEST_CASE("A search that finds nobody says so and leaves the panel alone", "[who]") {
    WhoList w;
    w.answered(1, 40, {entry("Ann")});
    auto none = w.answered(0, 39, {});
    CHECK(has<NoPlayersFound>(none));
    CHECK_FALSE(has<WhoListUpdated>(none));
    CHECK(w.results().empty());
    CHECK(w.onlineCount() == 39);
}

TEST_CASE("A new character starts without the last one's answer, but where answers go stays", "[who]") {
    WhoList w;
    w.showInPanel(true);
    w.answered(1, 40, {entry("Ann")});
    w.beginCharacter();
    CHECK(w.results().empty());
    CHECK(w.onlineCount() == 0);
    CHECK(w.toUI());
}

TEST_CASE("Asking needs the world", "[who]") {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    WhoService service{gateway, bus};
    service.queryWho("Ann");
    gateway.world = false;
    service.queryWho("Bob");
    CHECK(gateway.sent == std::vector<std::string>{"who Ann"});
}
