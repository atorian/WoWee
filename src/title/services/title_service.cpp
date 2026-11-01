#include "title/services/title_service.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"

namespace wowee::title {

namespace {

class NoTitles final : public ITitles {
public:
    const std::unordered_set<uint32_t>& getKnownTitleBits() const override { return known_; }
    int32_t getChosenTitleBit() const override { return -1; }
    std::string getFormattedTitle(uint32_t) const override { return {}; }
    std::string getFormattedTitleById(uint32_t) const override { return {}; }
    std::string getFormattedTitleFor(uint32_t, const std::string&) const override { return {}; }
    void sendSetTitle(int32_t) override {}
    void loadTitles() override {}

private:
    std::unordered_set<uint32_t> known_;
};

NoTitles kNoTitles;

} // namespace

ITitles& orNoTitles(ITitles* titles) {
    return titles ? *titles : kNoTitles;
}

TitleService::TitleService(ITitleData& data, ITitleGateway& gateway, core::EventBus& events)
    : data_(data), gateway_(gateway), events_(events) {}

void TitleService::ensureLoaded() const {
    if (loaded_ || !data_.ready()) return;
    loaded_ = true;
    catalog_.describe(data_.titles());
}

std::string TitleService::playerNameOr(const char* fallback) const {
    std::string name = gateway_.playerName();
    return name.empty() ? fallback : name;
}

std::string TitleService::getFormattedTitle(uint32_t bit) const {
    ensureLoaded();
    return catalog_.byBit(bit, playerNameOr("unknown"));
}

std::string TitleService::getFormattedTitleById(uint32_t id) const {
    ensureLoaded();
    return catalog_.byId(id, playerNameOr("unknown"));
}

std::string TitleService::getFormattedTitleFor(uint32_t bit, const std::string& name) const {
    ensureLoaded();
    return catalog_.byBit(bit, name);
}

void TitleService::sendSetTitle(int32_t bit) {
    if (!gateway_.online()) return;
    gateway_.sendSetTitle(bit);
    titles_.wear(bit);
}

void TitleService::onEarned(uint32_t bit, bool lost) {
    ensureLoaded();
    // The chat line speaks to the player, so a name not known yet is "you".
    const std::string title = catalog_.byBit(bit, playerNameOr("you"));
    publish(titles_.earned(bit, lost));
    LOG_INFO("SMSG_TITLE_EARNED: bit=", bit, " lost=", lost, " title='", title, "'");
    events_.publish(TitleEarned{bit, lost, title});
}

void TitleService::publish(const std::vector<TitleChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

} // namespace wowee::title
