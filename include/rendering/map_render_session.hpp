#pragma once

#include "core/job_group.hpp"
#include <memory>

namespace wowee::rendering {
class Renderer;

/// Owns the current scene's instance/streaming leases. The renderer and its
/// asset caches outlive it; no instance or incomplete upload does.
class MapRenderSession {
public:
    MapRenderSession(Renderer& renderer, core::JobGroup::Token token);
    ~MapRenderSession();
    MapRenderSession(const MapRenderSession&) = delete;
    MapRenderSession& operator=(const MapRenderSession&) = delete;
    /// First entry creates subrenderers asynchronously; adopt each once ready.
    void attach();
    void stopStreaming();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
