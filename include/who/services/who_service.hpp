#pragma once

#include "who/domain/who_list.hpp"
#include "who/services/ports.hpp"
#include "who/services/who.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::who {

class WhoService final : public IWho, public IWhoFeed {
public:
    WhoService(IWhoGateway& gateway, core::EventBus& events);

    [[nodiscard]] const std::vector<WhoEntry>& getWhoResults() const override { return list_.results(); }
    [[nodiscard]] uint32_t getWhoOnlineCount() const override { return list_.onlineCount(); }
    [[nodiscard]] bool isWhoToUI() const override { return list_.toUI(); }
    void setWhoToUI(bool toUI) override { list_.showInPanel(toUI); }
    void queryWho(const std::string& playerName = "") override;

    void onAnswer(uint32_t found, uint32_t onlineCount, std::vector<WhoEntry> entries) override;
    void beginCharacter() override { list_.beginCharacter(); }

private:
    IWhoGateway& gateway_;
    core::EventBus& events_;
    WhoList list_;
};

} // namespace wowee::who
