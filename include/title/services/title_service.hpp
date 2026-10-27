#pragma once

#include "title/domain/catalog.hpp"
#include "title/domain/titles.hpp"
#include "title/services/ports.hpp"
#include "title/services/titles.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::title {

class TitleService final : public ITitles, public ITitlesFeed {
public:
    TitleService(ITitleData& data, ITitleGateway& gateway, core::EventBus& events);

    [[nodiscard]] const std::unordered_set<uint32_t>& getKnownTitleBits() const override { return titles_.known(); }
    [[nodiscard]] int32_t getChosenTitleBit() const override { return titles_.chosen(); }
    [[nodiscard]] std::string getFormattedTitle(uint32_t bit) const override;
    [[nodiscard]] std::string getFormattedTitleById(uint32_t id) const override;
    [[nodiscard]] std::string getFormattedTitleFor(uint32_t bit, const std::string& name) const override;
    void sendSetTitle(int32_t bit) override;
    void loadTitles() override { ensureLoaded(); }

    void onEarned(uint32_t bit, bool lost) override;
    void onKnownTitles(uint8_t operation, const std::vector<uint16_t>& bits) override {
        publish(titles_.updated(operation, bits));
    }
    void onChosen(int32_t bit) override { titles_.wear(bit); }
    void leftWorld() override { titles_.leftWorld(); }

private:
    /// Latched only once the file can be read: one early call would otherwise
    /// disable it for the whole session.
    void ensureLoaded() const;
    /// The player's name, or `fallback` while it is not known.
    std::string playerNameOr(const char* fallback) const;
    void publish(const std::vector<TitleChange>& changes);

    ITitleData& data_;
    ITitleGateway& gateway_;
    core::EventBus& events_;
    mutable TitleCatalog catalog_;
    mutable bool loaded_ = false;
    Titles titles_;
};

} // namespace wowee::title
