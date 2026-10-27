#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "title/domain/catalog.hpp"

namespace wowee::title {

/// CharTitles.dbc. Implemented over the asset manager.
class ITitleData {
public:
    virtual ~ITitleData() = default;

    [[nodiscard]] virtual bool ready() const = 0;
    virtual std::vector<TitleRecord> titles() = 0;
};

/// Who the player is, and the request to wear a title.
class ITitleGateway {
public:
    virtual ~ITitleGateway() = default;

    [[nodiscard]] virtual bool online() const = 0;
    /// Empty while the name is not known yet.
    [[nodiscard]] virtual std::string playerName() const = 0;
    virtual void sendSetTitle(int32_t bit) = 0;
};

} // namespace wowee::title
