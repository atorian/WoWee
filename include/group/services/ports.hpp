#pragma once

#include <cstdint>
#include <string>

namespace wowee::group {

/// Requests to the server about the group. Implemented by the protocol layer.
class IGroupGateway {
public:
    virtual ~IGroupGateway() = default;

    /// In the world with a connection; requests are dropped otherwise.
    [[nodiscard]] virtual bool canSend() const = 0;
    [[nodiscard]] virtual uint64_t playerGuid() const = 0;

    virtual void sendInvite(const std::string& name) = 0;
    virtual void sendAccept() = 0;
    virtual void sendDecline() = 0;
    virtual void sendUninvite(const std::string& name) = 0;
    virtual void sendLeave() = 0;
    virtual void sendConvertToRaid() = 0;
    virtual void sendLeader(uint64_t guid) = 0;
    virtual void sendLootMethod(uint32_t method, uint64_t masterGuid, uint32_t threshold) = 0;
    virtual void sendAssignment(uint8_t assignment, uint64_t guid, bool apply) = 0;
    virtual void sendAssistant(uint64_t guid, bool apply) = 0;
    /// `group` is zero-based, as the server counts.
    virtual void sendSubgroup(const std::string& name, uint8_t group) = 0;
    virtual void sendSwapSubgroup(const std::string& first, const std::string& second) = 0;
    virtual void sendRaidMark(uint8_t icon, uint64_t guid) = 0;
    virtual void sendReadyCheck() = 0;
    virtual void sendReadyCheckAnswer(bool ready) = 0;
};

} // namespace wowee::group
