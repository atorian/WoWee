#pragma once

#include "core/world_reconnect.hpp"

namespace wowee {
namespace auth { class AuthHandler; }

namespace core {

/// The client's renewal of a lost session at the login server it logged in
/// through: connect, RECONNECT_CHALLENGE/PROOF with the last session's key
/// (AuthHandler::reconnect), then the realm list for where the realm is now.
/// No password is kept for it or asked for.
class LoginServerRenewal final : public WorldSessionRenewal {
public:
    explicit LoginServerRenewal(auth::AuthHandler& auth) : auth_(auth) {}

    void begin(const game::WorldLogin& last) override;
    Result poll(float deltaTime) override;
    [[nodiscard]] game::WorldLogin login() const override { return login_; }
    [[nodiscard]] std::string failure() const override { return failure_; }
    void cancel() override;

private:
    enum class Step { Idle, Connecting, Proving, Listing };

    Result fail(Result result, const std::string& why);

    auth::AuthHandler& auth_;
    Step step_ = Step::Idle;
    Result decided_ = Result::Pending;
    game::WorldLogin login_;
    std::string failure_;
};

} // namespace core
} // namespace wowee
