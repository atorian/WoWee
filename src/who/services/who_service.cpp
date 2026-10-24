#include "who/services/who_service.hpp"

#include "core/event_bus.hpp"

namespace wowee::who {

WhoService::WhoService(IWhoGateway& gateway, core::EventBus& events)
    : gateway_(gateway), events_(events) {}

void WhoService::queryWho(const std::string& playerName) {
    if (gateway_.online()) gateway_.sendQuery(playerName);
}

void WhoService::onAnswer(uint32_t found, uint32_t onlineCount, std::vector<WhoEntry> entries) {
    for (const auto& change : list_.answered(found, onlineCount, std::move(entries)))
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

} // namespace wowee::who
