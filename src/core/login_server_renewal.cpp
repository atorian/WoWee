#include "core/login_server_renewal.hpp"

#include "auth/auth_handler.hpp"
#include "auth/auth_packets.hpp"
#include "core/logger.hpp"
#include "core/realm_address.hpp"

namespace wowee::core {

void LoginServerRenewal::begin(const game::WorldLogin& last) {
    login_ = last;
    failure_.clear();
    decided_ = Result::Pending;
    const std::string host = auth_.getServerHost();
    const uint16_t port = auth_.getServerPort();
    if (host.empty() || last.account.empty() || last.sessionKey.empty()) {
        step_ = Step::Idle;
        decided_ = fail(Result::Refused,
                        "there is no session to renew: the last login left no login server, "
                        "account or session key");
        return;
    }
    LOG_INFO("Renewing the session of ", last.account, " at the login server ", host, ":", port);
    step_ = Step::Connecting;
    auth_.beginConnect(host, port);
}

WorldSessionRenewal::Result LoginServerRenewal::poll(float deltaTime) {
    if (decided_ != Result::Pending || step_ == Step::Idle) return decided_;
    auth_.update(deltaTime);
    using auth::AuthState;
    const AuthState state = auth_.getState();
    if (state == AuthState::FAILED) {
        step_ = Step::Idle;
        decided_ = fail(auth_.lastFailureWasRefusal() ? Result::Refused : Result::Unreachable,
                        auth_.lastFailureReason());
        return decided_;
    }
    switch (step_) {
        case Step::Connecting:
            if (state == AuthState::CONNECTED) {
                step_ = Step::Proving;
                auth_.reconnect(login_.account, login_.sessionKey);
            }
            break;
        case Step::Proving:
            if (state == AuthState::AUTHENTICATED) {
                step_ = Step::Listing;
                auth_.requestRealmList();
            }
            break;
        case Step::Listing:
            if (state != AuthState::REALM_LIST_RECEIVED) break;
            step_ = Step::Idle;
            decided_ = Result::Renewed;
            for (const auto& realm : auth_.getRealms()) {
                if (static_cast<uint32_t>(realm.id) != login_.realmId) continue;
                const RealmAddress address = resolveRealmAddress(realm.address);
                LOG_INFO("Session renewed; realm ", realm.name, " is at ", address.host, ":",
                         address.port);
                login_.host = address.host;
                login_.port = address.port;
                return decided_;
            }
            LOG_WARNING("Session renewed, but realm ", login_.realmId,
                        " is not in the realm list; reconnecting to its last address ",
                        login_.host, ":", login_.port);
            break;
        case Step::Idle:
            break;
    }
    return decided_;
}

void LoginServerRenewal::cancel() {
    if (step_ == Step::Idle) return;
    step_ = Step::Idle;
    decided_ = Result::Pending;
    auth_.disconnect();
}

WorldSessionRenewal::Result LoginServerRenewal::fail(Result result, const std::string& why) {
    failure_ = why.empty() ? std::string("the login server gave no reason") : why;
    LOG_WARNING("Renewing the session at the login server ",
                result == Result::Refused ? "was refused: " : "failed: ", failure_);
    return result;
}

} // namespace wowee::core
