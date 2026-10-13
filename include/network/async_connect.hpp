#pragma once

#include "network/net_platform.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wowee {
namespace net {

/// A TCP connection made off the main thread.
///
/// Both sockets connected in the frame: the name was resolved with a blocking
/// getaddrinfo and the connect waited in select for up to 5 s (login) or 10 s
/// (world). A login server that did not answer froze the client for the whole
/// wait, and the "Connecting..." it had just been told to show never reached
/// the screen. This does the same steps on a thread of its own and hands over
/// a connected, non-blocking socket when they are done.
///
/// A connect nobody waits for any more is abandoned, not joined: the thread
/// closes what it opened when it finishes, so destroying the owner never waits
/// on a server that is not answering.
class AsyncConnect {
public:
    enum class Status { Idle, Pending, Connected, Failed };

    AsyncConnect() = default;
    AsyncConnect(const AsyncConnect&) = delete;
    AsyncConnect& operator=(const AsyncConnect&) = delete;
    ~AsyncConnect() { cancel(); }

    /// Starts resolving and connecting; poll() says when it is done.
    void start(const std::string& host, uint16_t port, int timeoutSeconds) {
        cancel();
        ensureInit();
        auto job = std::make_shared<Job>();
        job_ = job;
        std::thread([job, host, port, timeoutSeconds]() {
            std::string error;
            const socket_t fd = connectBlocking(host, port, timeoutSeconds, error);
            std::lock_guard<std::mutex> lock(job->mutex);
            if (job->abandoned) {
                if (fd != INVALID_SOCK) closeSocket(fd);
                return;
            }
            job->fd = fd;
            job->error = std::move(error);
            job->done = true;
        }).detach();
    }

    [[nodiscard]] Status poll() {
        if (!job_) return Status::Idle;
        std::lock_guard<std::mutex> lock(job_->mutex);
        if (!job_->done) return Status::Pending;
        return job_->fd != INVALID_SOCK ? Status::Connected : Status::Failed;
    }

    /// The connected socket, once poll() has said Connected. Ownership passes
    /// to the caller and this goes back to Idle.
    [[nodiscard]] socket_t take() {
        if (!job_) return INVALID_SOCK;
        socket_t fd = INVALID_SOCK;
        {
            std::lock_guard<std::mutex> lock(job_->mutex);
            if (!job_->done) return INVALID_SOCK;
            fd = job_->fd;
            job_->fd = INVALID_SOCK;
        }
        job_.reset();
        return fd;
    }

    /// Why it failed, once poll() has said Failed.
    [[nodiscard]] std::string error() const {
        if (!job_) return {};
        std::lock_guard<std::mutex> lock(job_->mutex);
        return job_->error;
    }

    void cancel() {
        if (!job_) return;
        {
            std::lock_guard<std::mutex> lock(job_->mutex);
            if (job_->done) {
                if (job_->fd != INVALID_SOCK) closeSocket(job_->fd);
                job_->fd = INVALID_SOCK;
            } else {
                job_->abandoned = true;
            }
        }
        job_.reset();
    }

    /// Resolve, connect and wait, all blocking: for the thread above, or for
    /// a caller that has nothing else to do. The socket comes back connected
    /// and non-blocking, with Nagle off.
    static socket_t connectBlocking(const std::string& host, uint16_t port,
                                    int timeoutSeconds, std::string& error) {
        struct sockaddr_in addr;
        socket_t fd = openResolvedSocket(host, port, addr);
        if (fd == INVALID_SOCK) {
            error = "could not resolve " + host;
            return INVALID_SOCK;
        }
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            const int err = lastError();
            if (!isInProgress(err)) {
                error = errorString(err);
                closeSocket(fd);
                return INVALID_SOCK;
            }
            fd_set writefds, errfds;
            FD_ZERO(&writefds);
            FD_ZERO(&errfds);
            FD_SET(fd, &writefds);
            FD_SET(fd, &errfds);
            struct timeval tv;
            tv.tv_sec = timeoutSeconds;
            tv.tv_usec = 0;
            const int sel = ::select(static_cast<int>(fd) + 1, nullptr, &writefds, &errfds, &tv);
            if (sel <= 0) {
                error = "no answer within " + std::to_string(timeoutSeconds) + " s";
                closeSocket(fd);
                return INVALID_SOCK;
            }
            int sockErr = 0;
            socklen_t errLen = sizeof(sockErr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sockErr), &errLen);
            if (sockErr != 0) {
                error = errorString(sockErr);
                closeSocket(fd);
                return INVALID_SOCK;
            }
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        return fd;
    }

private:
    struct Job {
        std::mutex mutex;
        bool done = false;
        bool abandoned = false;
        socket_t fd = INVALID_SOCK;
        std::string error;
    };
    std::shared_ptr<Job> job_;
};

} // namespace net
} // namespace wowee
