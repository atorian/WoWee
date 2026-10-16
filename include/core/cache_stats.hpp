#pragma once

#include <cstddef>

namespace wowee {
namespace core {

/// How much a cache holds, as its owner counts it: what the resource HUD and
/// the resource log line show for it. Header-only, so the renderer, the asset
/// manager and the client can all report in it without depending on each
/// other.
struct CacheStats {
    /// The entries held.
    size_t entries = 0;
    /// What the owner counts them as costing: decoded file bytes for the asset
    /// manager, estimated GPU bytes for a texture cache.
    size_t bytes = 0;
    /// What the owner lets it grow to before it evicts; 0 when it has no limit.
    size_t budgetBytes = 0;
};

} // namespace core
} // namespace wowee
