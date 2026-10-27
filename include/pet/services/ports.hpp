#pragma once

#include <cstdint>
#include <string>

namespace wowee::pet {

/// Requests to the server about the pet and the stable. Implemented by the
/// protocol layer.
class IPetGateway {
public:
    virtual ~IPetGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    /// A connection to the world server, in the world or not.
    [[nodiscard]] virtual bool connected() const = 0;
    /// CMSG_PET_ACTION with a packed action; see pet/domain/actions.hpp.
    virtual void sendAction(uint64_t petGuid, uint32_t action, uint64_t targetGuid) = 0;
    virtual void sendAutocast(uint64_t petGuid, uint32_t spellId, bool on) = 0;
    virtual void sendRename(uint64_t petGuid, const std::string& name) = 0;
    virtual void sendAbandon(uint64_t petGuid) = 0;
    virtual void sendUnlearnTalents() = 0;
    virtual void requestStableList(uint64_t masterGuid) = 0;
    virtual void sendStable(uint64_t masterGuid, uint8_t slot) = 0;
    virtual void sendUnstable(uint64_t masterGuid, uint32_t petNumber) = 0;
    virtual void sendBuyStableSlot(uint64_t masterGuid) = 0;
};

} // namespace wowee::pet
