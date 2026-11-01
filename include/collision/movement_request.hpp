#pragma once
#include "collision/movement_query.hpp"
#include <vector>

namespace wowee::collision {
class MovementPendingObserver;
enum class MovementGeometryStatus { Ready, NotReady, Failed };
class MovementFacetSource {
public:
    virtual ~MovementFacetSource() = default;
    // Preserve original facet order. On an incomplete query, output contains
    // whatever geometry was collected, as in the original world service.
    // Failed is distinct from streaming NotReady; the source logs its cause.
    virtual MovementGeometryStatus query(const MovementFacetRequest& request,
        uint32_t flags, std::vector<MovementFacet>& output) = 0;
};
// Owned by the mover's dispatcher and retained between requests. GroundNormal
// uses the last collected solid facets even when a rooted request does not query.
struct MovementRequestCache {
    MovementFacetRequest request{};
    std::vector<MovementFacet> solids,liquids;
};
struct MovementRequestEvent {
    enum class Kind { StateChanged, SkipTime } kind;
    uint32_t time, remaining, oldFlags, oldFlags2;
    bool wasJumping;
};
struct MovementRequestResult {
    uint32_t elapsedMs = 0;
    MovementGeometryStatus geometryStatus = MovementGeometryStatus::Ready;
    std::vector<MovementRequestEvent> events;
    std::optional<uint64_t> transportRequest;
    std::optional<uint32_t> fallResetTime;
};

// Original CollideRequestMove on a mover without an attached transport or an
// active spline. Events are observed at CallMoveEventHandlers, before unit /
// protocol delivery; this offline entry does not claim packet equivalence or
// attach a transport. The caller supplies displacement, not visual interpolation.
[[nodiscard]] MovementRequestResult traceUnattachedMovementRequest(
    MovementFacetSource& world, MovementRequestCache& cache, SurfaceMovementState& state,
    const SurfaceMovementShape& shape, const glm::vec3& displacement,
    uint32_t timeMs, uint32_t elapsedMs, bool locallyControlled, bool ghost,
    MovementPendingObserver* pendingObserver=nullptr);
} // namespace wowee::collision
