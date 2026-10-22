#include "collision/movement_surface.hpp"
#include "movement_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>

namespace wowee::collision {
namespace {
using C3Vector = glm::vec3;
using C2Vector = glm::vec2;
using C4Plane = MovementPlane;
using CFacet = MovementFacet;
bool fnotequal(float a, float b) { return std::abs(double(a)-b) >= 9.5367432e-7f; }
bool fequal(float a, float b) { return !fnotequal(a,b); }
struct WOWGUID { uint64_t m_guid = 0; };
constexpr uint32_t MOVEMENTFLAG_FORWARD=1,MOVEMENTFLAG_BACKWARD=2,MOVEMENTFLAG_STRAFE_LEFT=4,MOVEMENTFLAG_STRAFE_RIGHT=8,
MOVEMENTFLAG_PITCH_UP=0x40,MOVEMENTFLAG_PITCH_DOWN=0x80,MOVEMENTFLAG_WALKING=0x100,MOVEMENTFLAG_ONTRANSPORT=0x200,MOVEMENTFLAG_ROOT=0x800,
MOVEMENTFLAG_FALLING=0x1000,MOVEMENTFLAG_FALLING_FAR=0x2000,MOVEMENTFLAG_PENDING_ROOT=0x100000,MOVEMENTFLAG_SWIMMING=0x200000,
MOVEMENTFLAG_ASCENDING=0x400000,MOVEMENTFLAG_DESCENDING=0x800000,MOVEMENTFLAG_FLYING=0x2000000,MOVEMENTFLAG_SPLINE_ELEVATION=0x4000000,
MOVEMENTFLAG_SAFE_FALL=0x20000000,MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING=0x20;

// Original functions retain their names for comparison with build 12340.
// See docs/original-collision-reference.md. Explicit double intermediates and
// float stores preserve the original x87 spill boundaries; do not reassociate.
struct SurfaceTraversal {
    std::span<const MovementFacet> facets;
    SurfaceMovementState& state;
    float m_collisionRadius, m_collisionHeight, m_stepUpHeight;
    bool clientControlled;
    std::optional<uint64_t> transportRequest;
    static constexpr float s_terminalVelocity = 60.148003f, s_safeFallTerminalVelocity = 7;
    static constexpr uint8_t s_volumeEdges[32] = {0,1,0,2,0,3,0,4,1,2,2,3,3,4,4,1,1,5,2,6,3,7,4,8,5,6,6,7,7,8,8,5};
    int DistanceToMove(const C3Vector& dir, float distance,
                       uint32_t* index, C4Plane* planes, uint32_t* count,
                       float* allowed) const {
        const auto hit = traceMovementBody(facets,
            MovementBody(state.position, m_collisionRadius, m_collisionHeight), dir, distance);
        *index = hit.facet;
        *count = hit.planeCount;
        *allowed = hit.distance;
        std::copy_n(hit.planes.data(), hit.planeCount, planes);
        return 1;
    }
    void IntersectPlanes(const C4Plane& a, const C4Plane& b, const C4Plane& c, C3Vector& point) {
        const auto matrix = glm::transpose(glm::mat3(a.normal,b.normal,c.normal));
        point = glm::inverse(matrix)*C3Vector(-a.offset,-b.offset,-c.offset);
    }
    void SetTransport(WOWGUID owner) { transportRequest = owner.m_guid; }
    // 0x0075e250
    C3Vector CalcRunWalkBlockingObstaclePush(const C3Vector& move, float dt, float maxDistance, const C3Vector& normal) const {
        if (normal.z < 0.0f && -normal.z > 0.64278764f) {
            return C3Vector(0.0f, 0.0f, 0.0f);
        }

        glm::dvec2 direction(normal);
        double lengthSq = direction.y * direction.y + direction.x * direction.x;

        if (lengthSq > 0.00000023841858f) {
            double scale = 1.0f / sqrt(lengthSq);
            direction.x = direction.x * scale;
            direction.y = direction.y * scale;
        }

        double push = (-double(normal.x) * move.x + (-double(normal.y) * move.y + -double(normal.z) * move.z)) * dt;
        double denominator = normal.y * direction.y + normal.x * direction.x + normal.z * 0.0f;
        double distance = fabs(denominator) < 0.00000023841858f ? push : push / denominator;
        float resultX = direction.x * (distance + 0.001f);
        float resultY = direction.y * (distance + 0.001f);

        C2Vector move2D(move);
        double moveX = double(move2D.x) * dt;
        double moveY = double(dt) * move2D.y;
        double totalX = moveX + resultX;
        double totalY = moveY + resultY;
        double totalSq = totalY * totalY + totalX * totalX;

        if (double(maxDistance) * maxDistance < totalSq) {
            double scale = maxDistance / sqrt(totalSq);
            resultX = totalX * scale - moveX;
            resultY = totalY * scale - moveY;
        }

        return C3Vector(resultX, resultY, 0.0f);
    }

    // 0x0075d740
    int32_t ComputeSteepSurfacePushNormalImpl(const C4Plane& plane) const {
        float top = m_collisionRadius * 1.849399f;
        C3Vector points[5];
        points[0] = state.position;
        points[1] = C3Vector(m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, state.position.z + top);
        points[2] = C3Vector(state.position.x - m_collisionRadius, m_collisionRadius + state.position.y, state.position.z + top);
        points[3] = C3Vector(m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, state.position.z + top);
        points[4] = C3Vector(-m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, top + state.position.z);

        for (uint32_t i = 0; i < 5; i++) {
            if (fabs(fabs(points[i].x * plane.normal.x + points[i].z * plane.normal.z + points[i].y * plane.normal.y + plane.offset)) < 0.0013888889f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075d890. The binary really takes fabs of both endpoint distances
    // before comparing their product with zero. Preserve this original
    // finite-input behaviour; replacing it with a signed-plane test changes
    // the falling-edge response.
    int32_t PlaneIntersectsVolume(const C4Plane& plane) const {
        float top = m_collisionRadius * 1.849399f;
        C3Vector points[9];
        points[0] = state.position;
        points[1] = C3Vector(m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, state.position.z + top);
        points[2] = C3Vector(-m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, state.position.z + top);
        points[3] = C3Vector(m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, state.position.z + top);
        points[4] = C3Vector(-m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, top + state.position.z);
        points[5] = C3Vector(m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);
        points[6] = C3Vector(state.position.x - m_collisionRadius, m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);
        points[7] = C3Vector(m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);
        points[8] = C3Vector(-m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);

        for (uint32_t i = 0; i < 32; i += 2) {
            const C3Vector& a = points[s_volumeEdges[i]];
            const C3Vector& b = points[s_volumeEdges[i + 1]];
            float distanceA = fabs(a.y * plane.normal.y + a.z * plane.normal.z + a.x * plane.normal.x + plane.offset);
            float distanceB = fabs(b.y * plane.normal.y + b.z * plane.normal.z + b.x * plane.normal.x + plane.offset);

            if (distanceA * distanceB < 0.0f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075db00
    C3Vector PushOffObstacleEdge(const C3Vector& point, const C3Vector& direction, uint32_t facet) const {
        const CFacet& data = facets[facet];

        C3Vector edge = data.vertices[1] - data.vertices[0];
        float lengthSq = edge.y * edge.y + edge.z * edge.z + edge.x * edge.x;

        if (lengthSq > 0.00000023841858f) {
            float scale = 1.0f / sqrt(lengthSq);
            edge.x = edge.x * scale;
            edge.y = edge.y * scale;
            edge.z = edge.z * scale;
        }

        float t = (point.z - data.vertices[0].z) * edge.z + (point.y - data.vertices[0].y) * edge.y + (point.x - data.vertices[0].x) * edge.x;
        float dz = point.z - (t * edge.z + data.vertices[0].z);
        float dy = point.y - (edge.y * t + data.vertices[0].y);
        float dx = point.x - (t * edge.x + data.vertices[0].x);
        float best = dx * dx + dy * dy + dz * dz;

        C3Vector edge1 = data.vertices[2] - data.vertices[1];
        lengthSq = edge1.z * edge1.z + edge1.y * edge1.y + edge1.x * edge1.x;

        if (lengthSq > 0.00000023841858f) {
            float scale = 1.0f / sqrt(lengthSq);
            edge1.x = edge1.x * scale;
            edge1.y = edge1.y * scale;
            edge1.z = scale * edge1.z;
        }

        t = (point.z - data.vertices[1].z) * edge1.z + (point.y - data.vertices[1].y) * edge1.y + (point.x - data.vertices[1].x) * edge1.x;
        dy = point.y - (edge1.y * t + data.vertices[1].y);
        dz = point.z - (edge1.z * t + data.vertices[1].z);
        dx = point.x - (edge1.x * t + data.vertices[1].x);
        float distance = dx * dx + (dz * dz + dy * dy);

        if (best > distance) {
            best = distance;
            edge = edge1;
        }

        C3Vector edge2 = data.vertices[0] - data.vertices[2];
        lengthSq = edge2.y * edge2.y + edge2.z * edge2.z + edge2.x * edge2.x;

        if (lengthSq > 0.00000023841858f) {
            float scale = 1.0f / sqrt(lengthSq);
            edge2.x = edge2.x * scale;
            edge2.y = edge2.y * scale;
            edge2.z = scale * edge2.z;
        }

        t = (point.z - data.vertices[2].z) * edge2.z + (point.y - data.vertices[2].y) * edge2.y + (point.x - data.vertices[2].x) * edge2.x;
        dx = point.x - (edge2.x * t + data.vertices[2].x);
        dy = point.y - (edge2.y * t + data.vertices[2].y);
        dz = point.z - (edge2.z * t + data.vertices[2].z);

        if (dx * dx + dz * dz + dy * dy < best) {
            edge = edge2;
        }

        C3Vector push(data.plane.normal.z * edge.y - data.plane.normal.y * edge.z, edge.z * data.plane.normal.x - data.plane.normal.z * edge.x, edge.x * data.plane.normal.y - edge.y * data.plane.normal.x);

        if (direction.z * push.z + direction.y * push.y + push.x * direction.x < 0.0f) {
            push = C3Vector(-push.x, -push.y, -push.z);
        }

        return push;
    }

    // 0x0075cfe0
    int32_t IsFacetOverhead(uint32_t facet) const {
        float left = state.position.x - m_collisionRadius;
        float right = m_collisionRadius + state.position.x;
        float front = state.position.y - m_collisionRadius;
        float back = m_collisionRadius + state.position.y;
        float top = m_collisionHeight + state.position.z;
        C3Vector corners[4] = { C3Vector(left, front, top), C3Vector(right, front, top), C3Vector(left, back, top), C3Vector(right, back, top) };
        const CFacet& data = facets[facet];

        for (uint32_t i = 0; i < 4; i++) {
            if (fabs(double(corners[i].z) * data.plane.normal.z + double(corners[i].x) * data.plane.normal.x + double(corners[i].y) * data.plane.normal.y + data.plane.offset) < 0.0013888889f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075d2d0
    int32_t IsFacetWall(uint32_t facet) const {
        const CFacet& data = facets[facet];

        if (data.plane.normal.z > 0.64278764f) {
            return 0;
        }

        return data.plane.normal.z >= 0.0f || -data.plane.normal.z <= 0.64278764f;
    }

    // 0x006e9520
    double GetRemainingStepUpHeight() const {
        double height = clientControlled ? m_stepUpHeight : 2.0f;

        if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
            height -= double(state.position.z) - state.stepStartZ;
        }

        if (height <= 0.0f) {
            return 0.0f;
        }

        return height;
    }

    // 0x006e8fc0
    float GetStepUpHeight() const {
        if (!clientControlled) {
            return 2.0f;
        }

        return m_stepUpHeight;
    }

    // 0x0075b4f0
    void RestoreMoveState(const SurfaceMovementState& saved) {
        state.anchorPosition = saved.anchorPosition;
        state.anchorFacing = saved.anchorFacing;
        state.anchorPitch = saved.anchorPitch;
        state.anchorElapsedMs = saved.anchorElapsedMs;
        state.fallTimeMs = saved.fallTimeMs;
        state.direction = saved.direction;
        state.direction2D = saved.direction2D;
        state.flags = saved.flags;
        state.currentSpeed = saved.currentSpeed;
    }

    // 0x0075b480
    void SaveMoveState(SurfaceMovementState& saved) const {
        saved.anchorPosition = state.anchorPosition;
        saved.anchorFacing = state.anchorFacing;
        saved.anchorPitch = state.anchorPitch;
        saved.anchorElapsedMs = state.anchorElapsedMs;
        saved.fallTimeMs = state.fallTimeMs;
        saved.direction = state.direction;
        saved.direction2D = state.direction2D;
        saved.flags = state.flags;
        saved.currentSpeed = state.currentSpeed;
    }

    // 0x0075e040
    int32_t CheckFallImpactThreshold(float timeFallen, float elapsed, const C3Vector& move, int32_t force) const {
        if (force) {
            return 1;
        }

        if (state.fallVelocity < 0.0f) {
            float peak = TimeToJumpPeak();

            if (elapsed + timeFallen < peak) {
                return 1;
            }

            if (timeFallen > peak) {
                return 0;
            }

            float distance = (peak - timeFallen) * state.currentSpeed;

            if (move.x * move.x + move.y * move.y < distance * distance) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075eb00
    float GetTimeJustFallen(float elapsed, float timeFallen, C3Vector* move, float* distance, int32_t force, float speed) {
        int32_t impact = CheckFallImpactThreshold(timeFallen, elapsed, *move, force);
        float moveTime = 0.0f;

        if (speed > 9.5367432e-7f) {
            C2Vector move2D(*move);
            moveTime = sqrt(move2D.x * move2D.x + move2D.y * move2D.y) / speed * elapsed;
        }

        float fallTime = CalcTimeFallen(state.fallStartZ - state.position.z - move->z, impact);

        if (!(timeFallen < fallTime)) {
            move->x = 0.0f;
            move->y = 0.0f;
            move->z = 0.0f;
            *distance = 0.0f;
            return 0.0f;
        }

        float remaining = fallTime - timeFallen;

        if (elapsed < remaining) {
            return elapsed;
        }

        if (moveTime > remaining) {
            float scale = remaining / moveTime;
            move->x = move->x * scale;
            move->y = scale * move->y;
            *distance = sqrt(move->x * move->x + move->y * move->y + move->z * move->z);
        }

        return remaining;
    }

    // 0x0075e0c0
    C3Vector CalcRunWalkWalkableObstaclePush(const C3Vector& move, float* scale, C3Vector normal, int32_t useAltNormal, const C3Vector& altNormal) const {
        if (useAltNormal && normal.z <= 0.64278764f && !(fabs(altNormal.z * altNormal.z + altNormal.y * altNormal.y + altNormal.x * altNormal.x) < 2.3841858e-7f)) {
            normal.x = -altNormal.x;
            normal.y = -altNormal.y;
            normal.z = -altNormal.z;
        }

        double push = (-double(normal.x) * move.x + (-double(normal.y) * move.y + -double(normal.z) * move.z)) * *scale;
        float height;

        if (fabs(normal.z) < 2.3841858e-7f) {
            height = push >= 0.0f ? 3.4028235e38f : -3.4028235e38f;
        } else {
            height = push / normal.z;
        }

        float result;

        if ((state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) && height < 0.0f) {
            *scale = 0.0f;
            result = GetRemainingStepUpHeight();
        } else if (GetRemainingStepUpHeight() >= height) {
            result = height;

            if (height < -GetStepUpHeight()) {
                *scale = -(GetStepUpHeight() * (double(*scale) / height));
                result = -GetStepUpHeight();
            }
        } else {
            *scale = GetRemainingStepUpHeight() * (double(*scale) / height);
            result = GetRemainingStepUpHeight();
        }

        return C3Vector(0.0f * result, 0.0f * result, result);
    }

    // 0x0075b690
    int32_t IsSurfaceTooSteep(uint32_t facet) const {
        return facets[facet].plane.normal.z <= 0.64278764f;
    }

    // 0x0075d0a0
    int32_t WalkableFacetEnclosesPoint(uint32_t facet, const C3Vector& point) const {
        C4Plane planes[4];

        for (uint32_t i = 0; i < 4; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        C3Vector up(0.0f, 0.0f, 1.0f);
        uint8_t indices[3] = { 0, 1, 2 };
        const CFacet& data = facets[facet];
        // The original consumes the initialized planes even if extrusion stops
        // early on a degenerate edge. Keep the same footprint semantics.
        (void)detail::extrudeMovementFace(data.vertices, indices, up, planes);

        for (uint32_t i = 0; i < 3; i++) {
            if (double(planes[i].normal.z) * point.z + double(planes[i].normal.x) * point.x + double(planes[i].normal.y) * point.y + planes[i].offset > 0.083333336f) {
                return 0;
            }
        }

        return 1;
    }

    // 0x0075d340
    int32_t IsSlopeFallable(uint32_t facet, const C3Vector& move) const {
        C3Vector point(state.position.x + move.x, state.position.y + move.y, state.position.z + move.z);
        float threshold = clientControlled ? 0.64278764f : 0.17364818f;
        return threshold < facets[facet].plane.normal.z && WalkableFacetEnclosesPoint(facet, point);
    }

    // 0x0075d4b0
    void ComputeSteepSurfacePushNormalHelper(uint32_t facet, const C3Vector& direction, C3Vector& edge, const C3Vector& point) {
        uint32_t next[3] = { 1, 2, 0 };
        float best = 3.4028235e38f;
        edge = C3Vector(0.0f, 0.0f, 0.0f);
        const CFacet& data = facets[facet];

        for (uint32_t i = 0; i < 3; i++) {
            const C3Vector& v = data.vertices[i];
            const C3Vector& w = data.vertices[next[i]];
            float ex = v.x - w.x;
            float ey = v.y - w.y;
            float ez = v.z - w.z;
            float length = sqrt(ez * ez + ey * ey + ex * ex);

            if (fabs(length) < 0.00000023841858f) {
                continue;
            }

            float scale = 1.0f / length;
            C3Vector n(ex * scale, ey * scale, scale * ez);
            float distSq;

            if (fabs(direction.z * n.z + direction.y * n.y + direction.x * n.x - 1.0f) >= 0.00000095367432f) {
                float cx = direction.z * n.y - direction.y * n.z;
                float cy = n.z * direction.x - direction.z * n.x;
                float cz = n.x * direction.y - n.y * direction.x;
                float d = -(point.z * cz + point.y * cy + point.x * cx);
                float dist = cz * v.z + (cx * v.x + cy * v.y) + d;
                distSq = dist * dist;
            } else {
                float t = (v.x - point.x) * direction.x + (v.y - point.y) * direction.y + (v.z - point.z) * direction.z;
                float dx = v.x - (direction.x * t + point.x);
                float dy = v.y - (direction.y * t + point.y);
                float dz = v.z - (t * direction.z + point.z);
                distSq = dz * dz + dy * dy + dx * dx;
            }

            if (distSq < best) {
                best = distSq;
                edge = n;
            }
        }
    }

    // 0x0075de40
    int32_t CheckFallingConditions(const C4Plane* planes, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            if (fabs(planes[i].normal.z - 1.0f) < 0.00000095367432f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075de80
    int32_t GetBoxPushNormal(const C4Plane* planes, uint32_t count, C3Vector& normal) {
        if (!count || count > 4) {
            return 0;
        }

        for (uint32_t i = 0; i < count; i++) {
            if (fabs(planes[i].normal.z - -0.4756366f) >= 0.00000095367432f) {
                return 0;
            }
        }

        switch (count) {
            case 1:
                normal = planes[0].normal;
                break;

            case 2:
                normal.x = (planes[1].normal.x + planes[0].normal.x) * 0.638556f;
                normal.y = (planes[1].normal.y + planes[0].normal.y) * 0.638556f;
                normal.z = 0.638556f * (planes[1].normal.z + planes[0].normal.z);
                break;

            case 3: {
                uint32_t flatY[3];
                uint32_t flatX[3];
                uint32_t flatYCount = 0;
                uint32_t flatXCount = 0;

                for (uint32_t i = 0; i < 3; i++) {
                    if (fabs(planes[i].normal.y) < 0.00000095367432f) {
                        flatY[flatYCount++] = i;
                    } else if (fabs(planes[i].normal.x) < 0.00000095367432f) {
                        flatX[flatXCount++] = i;
                    }
                }

                normal = planes[flatYCount == 1 ? flatY[0] : flatX[0]].normal;
                break;
            }

            case 4:
                normal.x = 0.0f;
                normal.y = 0.0f;
                normal.z = 0.0f;
                break;
        }

        return 1;
    }

    // 0x0075e760
    C3Vector GetSteepSurfacePushNormal(uint32_t facet, const C4Plane* planes, uint32_t count) {
        if (count == 1) {
            if (PlaneIntersectsVolume(facets[facet].plane)) {
                return C3Vector(-planes[0].normal.x, -planes[0].normal.y, -planes[0].normal.z);
            }
        } else if (count == 2) {
            float x = planes[1].normal.z * planes[0].normal.y - planes[0].normal.z * planes[1].normal.y;
            float y = planes[1].normal.x * planes[0].normal.z - planes[0].normal.x * planes[1].normal.z;
            float z = planes[0].normal.x * planes[1].normal.y - planes[1].normal.x * planes[0].normal.y;
            float scale = 1.0f / sqrt(y * y + x * x + z * z);
            C3Vector direction(x * scale, y * scale, z * scale);
            const CFacet& data = facets[facet];

            if (fabs(direction.z) >= 0.00000095367432f && fnotequal(direction.y * data.plane.normal.y + direction.z * data.plane.normal.z + direction.x * data.plane.normal.x, 0.0f) && (fabs(direction.z - 1.0f) < 0.00000095367432f || !ComputeSteepSurfacePushNormalImpl(data.plane))) {
                C3Vector point{0};
                IntersectPlanes(facets[facet].plane, planes[0], planes[1], point);
                C3Vector edge{0};
                ComputeSteepSurfacePushNormalHelper(facet, direction, edge, point);
                C3Vector normal = glm::cross(direction, edge);
                float length = sqrt(normal.y * normal.y + normal.z * normal.z + normal.x * normal.x);

                if (fequal(length, 0.0f)) {
                    return -planes[0].normal;
                }

                normal /= length;

                if (planes[0].normal.x * normal.x + normal.y * planes[0].normal.y + planes[0].normal.z * normal.z <= 0.0f) {
                    return normal;
                }

                return C3Vector(-normal.x, -normal.y, -normal.z);
            }
        }

        return facets[facet].plane.normal;
    }

    // 0x0075e9c0
    C2Vector CalcFallObstaclePush(const C3Vector& direction, float distance, float maxDistance, uint32_t facet, const C4Plane* planes, uint32_t count) {
        C3Vector normal{0};

        if (IsFacetWall(facet)) {
            normal = GetSteepSurfacePushNormal(facet, planes, count);
        } else {
            C3Vector point(direction.x * distance + state.position.x, direction.y * distance + state.position.y, distance * direction.z + state.position.z);
            normal = PushOffObstacleEdge(point, direction, facet);
        }

        C2Vector push(normal);
        float lengthSq = push.y * push.y + push.x * push.x;

        if (lengthSq > 0.00000023841858f) {
            float scale = 1.0f / sqrt(lengthSq);
            push.x = push.x * scale;
            push.y = push.y * scale;
        }

        float d = (-normal.x * direction.x + -normal.y * direction.y + -normal.z * direction.z) * (maxDistance - distance);
        float denom = normal.x * push.x + normal.y * push.y + normal.z * 0.0f;
        float scale = fabs(denom) < 0.00000023841858f ? d : d / denom;
        return C2Vector(push.x * (scale + 0.001f), push.y * (scale + 0.001f));
    }

    // 0x0075d1c0
    uint32_t UseWalkableRedirection(uint32_t facet, int32_t* cleared) {
        if (cleared) {
            *cleared = 0;
        }

        float threshold = clientControlled ? 0.64278764f : 0.17364818f;

        if (threshold < facets[facet].plane.normal.z) {
            if (WalkableFacetEnclosesPoint(facet, state.position)) {
                state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            }

            return 1;
        }

        if (facets[facet].plane.normal.z < 0.0f && IsFacetOverhead(facet)) {
            if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
                if (cleared) {
                    *cleared = 1;
                }

                state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            }

            return -facets[facet].plane.normal.z > 0.64278764f;
        }

        return state.flags & MOVEMENTFLAG_SPLINE_ELEVATION;
    }

    // 0x00760fc0
    int32_t TryFallingDown(float timeFallen, C3Vector* move, float* distance, C2Vector* push, uint64_t* facetId, int32_t* onSlope, int32_t* landed) {
        *landed = 0;
        *onSlope = 0;
        float length = *distance;

        if (fabs(length) < 0.00000023841858f) {
            *facetId = 0;
            return 0;
        }

        C4Plane planes[9];

        for (uint32_t i = 0; i < 7; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        C3Vector direction(move->x * (1.0f / length), move->y * (1.0f / length), 1.0f / length * move->z);
        uint32_t facetIndex;
        uint32_t planeCount;

        if (!DistanceToMove(direction, length, &facetIndex, planes, &planeCount, distance)) {
            move->x = 0.0f;
            move->y = 0.0f;
            move->z = 0.0f;
            return 2;
        }

        if (facetIndex == facets.size()) {
            *facetId = 0;
            return 0;
        }

        float d = *distance;
        move->x = direction.x * d;
        move->y = direction.y * d;
        move->z = d * direction.z;
        *facetId = facets[facetIndex].owner;

        if (IsSlopeFallable(facetIndex, *move)) {
            *onSlope = 1;
            return 1;
        }

        if (direction.z > 0.0f) {
            float peak = TimeToJumpPeak() - timeFallen;

            if (peak >= 0.0f && peak * state.currentSpeed >= *distance && CheckFallingConditions(planes, planeCount)) {
                *landed = 1;
                *facetId = 0;
                return 1;
            }
        }

        *push = CalcFallObstaclePush(direction, *distance, length, facetIndex, planes, planeCount);
        return 1;
    }

    // 0x007612b0
    uint32_t FallDown(uint32_t elapsed, C3Vector move) {
        float timeFallen = 0.0f;
        float totalTime = elapsed * 0.001f;
        float startTime = 0.001f * static_cast<float>(state.fallTimeMs);
        int32_t stuck = 0;
        uint64_t transport = 0;
        C2Vector push(0.0f, 0.0f);
        int32_t slid = 0;
        int32_t flattened = 0;
        C2Vector move2D(move);
        int32_t landed = 0;
        float length = sqrt(move.y * move.y + move.x * move.x + move.z * move.z);
        float length2D = sqrt(move2D.y * move2D.y + move2D.x * move2D.x);
        float vertical = fabs(move.z);
        C2Vector start(state.position);
        int32_t iterations = 0;
        float startLength2D = length2D;
        float endTime;
        int32_t onSlope;

        while (true) {
            if (fabs(length) < 0.00000095367432f) {
                endTime = totalTime;
                goto finish;
            }

            {
                float distance = length;
                float scale = 1.0f / length;
                C3Vector direction(move.x * scale, move.y * scale, move.z * scale);
                float fallTime = startTime + timeFallen;
                int32_t result = TryFallingDown(fallTime, &move, &distance, &push, &transport, &onSlope, &landed);



                float dt = result ? GetTimeJustFallen(totalTime - timeFallen, fallTime, &move, &distance, landed, length2D) : totalTime - timeFallen;
                state.position.x = state.position.x + move.x;
                state.position.y = state.position.y + move.y;
                state.position.z = state.position.z + move.z;
                timeFallen = timeFallen + dt;

                if (landed) {
                    state.flags |= MOVEMENTFLAG_FALLING_FAR;
                    endTime = timeFallen;
                    state.fallTimeMs = 0;
                    state.fallVelocity = 0.0f;
                    state.fallStartZ = state.position.z;
                    goto finish;
                }

                if (onSlope) {
                    goto stop;
                }

                if (timeFallen + 0.00050000002f >= totalTime) {
                    endTime = totalTime;
                    goto finish;
                }

                slid = 1;
                float left = length - distance;

                if (dt > 0.00050000002f) {
                    stuck = 1;
                } else if (++stuck > 5) {
                    if (flattened || !fnotequal(length2D, 0.0f)) {
                        goto stop;
                    }

                    stuck = 0;
                    iterations++;
                    state.currentSpeed = 0.0f;
                    flattened = 1;
                    move.x = 0.0f;
                    move.y = 0.0f;
                    move.z = direction.z * left;
                    length = fabs(move.z);
                    continue;
                }

                move.x = direction.x * left + push.x;
                move.y = direction.y * left + push.y;
                move.z = left * direction.z;

                if (iterations) {
                    C2Vector position(state.position);
                    float tx = position.x - start.x;
                    float ty = position.y - start.y;
                    C2Vector next(move);
                    float dy = next.y + ty;
                    float dx = next.x + tx;
                    float distSq = dx * dx + dy * dy;

                    if ((startLength2D + 0.00000095367432f) * (startLength2D + 0.00000095367432f) < distSq && vertical * 1.1866661f * (vertical * 1.1866661f) < distSq) {
                        goto stop;
                    }
                }

                move2D = C2Vector(move);
                length2D = sqrt(move2D.y * move2D.y + move2D.x * move2D.x);

                if (fabs(length2D) < 0.00000095367432f) {
                    state.currentSpeed = 0.0f;
                } else {
                    float inverse = 1.0f / length2D;
                    move2D.x = move2D.x * inverse;
                    move2D.y = move2D.y * inverse;
                    float speed = (move2D.x * state.direction2D.x + move2D.y * state.direction2D.y) * state.currentSpeed;
                    state.currentSpeed = speed;

                    if (!(speed > 0.0f) && !(0.0f == speed)) {
                        state.currentSpeed = 0.0f;
                    }
                }

                iterations++;
                state.direction2D = move2D;
                length = sqrt(move.y * move.y + move.x * move.x + move.z * move.z);
                state.direction = C3Vector(state.direction2D.x, state.direction2D.y, 0.0f);
            }
        }

    stop:
        StopFalling();
        endTime = timeFallen;

    finish:
        uint32_t used = static_cast<uint32_t>(endTime * 1000.0f);

        if (!landed) {
            state.fallTimeMs += used;
        }



        if (slid) {
            UpdateAnchors(0);
        }

        return used;
    }



    // 0x007619c0. Save/RestoreMoveState restores anchors and direction, not
    // the speculative position or fall origin. This routine inspects that
    // endpoint; AttemptStepUp then restores the feet. Do not replace the
    // original snapshot with a wholesale copy/restore of the public state.
    int32_t WillPassObstacle(const C2Vector& direction, const C3Vector& position, float height) {
        uint32_t time = static_cast<uint32_t>(sqrt((height + height) * 0.051837362f) * 1000.0f);
        SurfaceMovementState saved;
        SaveMoveState(saved);
        StartFalling(0.0f);
        float fallen = RelDistanceFallen(time);
        C3Vector move(state.direction2D.x, state.direction2D.y, -fallen);

        for (uint32_t t = 0; t < time; t += FallDown(time - t, move)) {
            if (!(state.flags & MOVEMENTFLAG_FALLING)) {
                break;
            }
        }

        RestoreMoveState(saved);
        C2Vector from(position);
        C2Vector to(state.position);
        float dx = to.x - from.x;
        float dy = to.y - from.y;
        float length = sqrt(dy * dy + dx * dx);
        return length >= m_collisionRadius && dx * (1.0f / length) * direction.x + dy * (1.0f / length) * direction.y > 0.98480773f;
    }

    // 0x00761b00
    int32_t AttemptStepUp(const C2Vector& direction, C3Vector normal) {
        C3Vector saved = state.position;
        C4Plane planes[9];

        for (uint32_t i = 0; i < 7; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        C2Vector move = direction;
        float reach = m_collisionRadius + 0.0013888889f;

        if (reach < GetStepUpHeight() * 1.1917536f) {
            reach = GetStepUpHeight() * 1.1917536f;
        }

        uint32_t facetIndex;
        uint32_t planeCount;
        float moveDist;

        if (normal.z <= 0.64278764f && normal.z >= 0.0f) {
            float scale = 1.0f / sqrt(normal.x * normal.x + normal.y * normal.y);
            move.x = -(normal.x * scale);
            move.y = -(normal.y * scale);
            C3Vector probe(move.x, move.y, 0.0f);

            if (!DistanceToMove(probe, reach, &facetIndex, planes, &planeCount, &moveDist)) {
                return 0;
            }

            if (facetIndex == facets.size() || facets[facetIndex].plane.normal != normal) {
                move = direction;
            }
        }

        C3Vector up(0.0f, 0.0f, 1.0f);
        float upDist;

        if (!DistanceToMove(up, GetRemainingStepUpHeight(), &facetIndex, planes, &planeCount, &upDist)) {
            return 0;
        }

        float rise = upDist;

        if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
            rise = state.position.z - state.stepStartZ + upDist;
        }

        if (fabs(rise) < 0.00000023841858f) {
            state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            return 1;
        }

        state.position.z = upDist + state.position.z;
        C3Vector forward(move.x, move.y, 0.0f);

        if (!DistanceToMove(forward, reach, &facetIndex, planes, &planeCount, &moveDist)) {
            return 0;
        }

        state.position.x = move.x * moveDist + state.position.x;
        state.position.y = move.y * moveDist + state.position.y;

        int32_t cleared;

        if (fnotequal(moveDist, reach) && UseWalkableRedirection(facetIndex, &cleared)) {
            float remaining = reach - moveDist;
            C3Vector boxNormal{0};
            int32_t hasBox = GetBoxPushNormal(planes, planeCount, boxNormal);
            C3Vector moveDir(move.x, move.y, 0.0f);
            C3Vector push = CalcRunWalkWalkableObstaclePush(moveDir, &remaining, facets[facetIndex].plane.normal, hasBox, boxNormal);
            C3Vector slide(move.x * remaining + push.x, remaining * move.y + push.y, push.z);
            float length = sqrt(slide.x * slide.x + slide.y * slide.y + push.z * push.z);

            if (fnotequal(length, 0.0f)) {
                slide /= length;
                float slideDist;

                if (!DistanceToMove(slide, length, &facetIndex, planes, &planeCount, &slideDist)) {
                    return 0;
                }

                slide *= slideDist;
                C2Vector flat(slide);
                moveDist = sqrt(flat.y * flat.y + flat.x * flat.x) + moveDist;
                state.position += slide;
                upDist = slide.z + upDist;
            }
        }

        if (facetIndex != facets.size() && m_collisionRadius >= moveDist) {
            state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            state.position = saved;
            return 1;
        }

        C3Vector down(0.0f, 0.0f, -1.0f);
        float downDist;

        if (!DistanceToMove(down, upDist, &facetIndex, planes, &planeCount, &downDist)) {
            return 0;
        }

        state.position.z = state.position.z - downDist;

        if (facetIndex == facets.size() || !IsSurfaceTooSteep(facetIndex) || WillPassObstacle(move, saved, upDist - downDist)) {
            state.position = saved;

            if (!(state.flags & MOVEMENTFLAG_SPLINE_ELEVATION)) {
                state.stepStartZ = saved.z;
            }

            state.flags |= MOVEMENTFLAG_SPLINE_ELEVATION;
            return 1;
        }

        state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
        state.position = saved;
        return 1;
    }

    // 0x007620f0
    uint32_t TraceSurface(uint32_t elapsed, float distance, const C2Vector& direction) {
        if (fabs(distance) < 0.00000095367432f) {
            return elapsed;
        }

        if (!facets.size()) {
            return StartFalling(0.0f) ? 0 : elapsed;
        }

        C4Plane planes[9];

        for (uint32_t i = 0; i < 7; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        float total = static_cast<float>(elapsed);
        float timeLeft = total;
        float timeUsed = 0.0f;
        int32_t stuck = 0;
        int32_t cleared = 0;
        int32_t moved = 0;
        float remaining = distance;
        float length2D = distance;
        C2Vector dir2D = direction;
        float climbBudget = distance / m_collisionRadius;
        C3Vector moveDir(direction.x, direction.y, 0.0f);
        uint32_t facetIndex;
        uint32_t planeCount;
        float moveDist;
        WOWGUID transport;

        if (!DistanceToMove(moveDir, distance, &facetIndex, planes, &planeCount, &moveDist)) {
            return elapsed;
        }

        while (true) {
            glm::dvec3 step=glm::dvec3(moveDir)*double(moveDist);

            if (climbBudget >= step.z) {
                climbBudget = climbBudget - step.z;
            } else {
                moved = 1;
                double scale = climbBudget / step.z;
                step.x = step.x * scale;
                step.y = step.y * scale;
                step.z = step.z * scale;
                moveDist = moveDist * scale;
                climbBudget = 0.0f;
            }

            state.position.x = step.x + state.position.x;
            state.position.y = step.y + state.position.y;
            state.position.z = step.z + state.position.z;

            if (facetIndex >= facets.size()) {
                const double used = double(timeLeft) + timeUsed;
                timeUsed = float(used);
                const double left = total - used;

                if (left >= 1.0f) {
                    timeLeft = left;
                    dir2D = direction;
                    remaining = left / total * distance;
                    moveDir = C3Vector(direction.x, direction.y, 0.0f);
                    goto next;
                }

                C3Vector down(0.0f, 0.0f, -1.0f);
                float downDist;

                if (!DistanceToMove(down, distance * 1.849399f, &facetIndex, planes, &planeCount, &downDist)) {
                    return elapsed;
                }

                state.position.z = state.position.z - downDist;

                if (facetIndex == facets.size()) {
                    transport = WOWGUID();

                    if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
                        goto finish;
                    }
                } else {
                    transport.m_guid = facets[facetIndex].owner;

                    if (!IsSurfaceTooSteep(facetIndex)) {
                        state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
                        goto finish;
                    }

                    const CFacet& facet = facets[facetIndex];

                    if ((state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) && facet.plane.normal.y * dir2D.y + facet.plane.normal.x * dir2D.x <= -0.0000099999997f) {
                        goto finish;
                    }
                }

                goto fall;
            }

            {
                transport.m_guid = facets[facetIndex].owner;
                float timeStep = double(moveDist) / remaining * timeLeft;
                remaining = remaining - moveDist;
                C2Vector flat{C3Vector(step)};
                length2D = length2D - sqrt(double(flat.y) * flat.y + double(flat.x) * flat.x);

                if (timeStep < 1.0f) {
                    if (++stuck > 5) {
                        moved = 1;
                        goto finish;
                    }
                } else {
                    stuck = 1;
                }

                timeUsed = timeUsed + timeStep;
                timeLeft = timeLeft - timeStep;

                if (total - timeUsed < 1.0f) {
                    if (fnotequal(total, timeUsed)) {
                        moved = 1;
                    }

                    goto finish;
                }

                C3Vector normal = facets[facetIndex].plane.normal;
                C3Vector push{0};
                C3Vector boxNormal{0};
                int32_t hasBox;

                if (UseWalkableRedirection(facetIndex, &cleared)) {
                    hasBox = GetBoxPushNormal(planes, planeCount, boxNormal);
                } else {
                    if (cleared) {
                        goto fall;
                    }

                    moved = 1;
                    hasBox = GetBoxPushNormal(planes, planeCount, boxNormal);
                    uint32_t wasStepping = state.flags & MOVEMENTFLAG_SPLINE_ELEVATION;

                    if (!AttemptStepUp(dir2D, normal)) {
                        return elapsed;
                    }

                    if (!(state.flags & MOVEMENTFLAG_SPLINE_ELEVATION)) {
                        if (wasStepping) {
                            goto fall;
                        }

                        push = CalcRunWalkBlockingObstaclePush(moveDir, remaining, length2D, normal);
                        goto slide;
                    }
                }

                {
                    float before = remaining;
                    push = CalcRunWalkWalkableObstaclePush(moveDir, &remaining, normal, hasBox, boxNormal);
                    timeLeft = double(remaining) / before * timeLeft;
                }

            slide:

                glm::dvec3 next=glm::dvec3(moveDir)*double(remaining)+glm::dvec3(push);
                moveDir = next;
                remaining = sqrt(next.y * next.y + next.z * next.z + next.x * next.x);

                if (remaining < 0.001f) {
                    moved = 1;
                    goto finish;
                }

                dir2D = C2Vector(moveDir);
                double scale = 1.0 / remaining;
                moveDir.x = moveDir.x * scale;
                moveDir.y = moveDir.y * scale;
                moveDir.z = scale * moveDir.z;
                float previous = length2D;
                const double flatLength = sqrt(double(dir2D.y) * dir2D.y + double(dir2D.x) * dir2D.x);
                length2D = float(flatLength);

                if (fabs(length2D) >= 0.00000023841858f) {
                    double inverse = 1.0 / flatLength;
                    dir2D.x = dir2D.x * inverse;
                    dir2D.y = dir2D.y * inverse;
                }

                if (previous - length2D > 0.00000095367432f) {
                    moved = 1;
                }

                if ((state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) && next.z > 0.00000095367432f) {
                    float top = float(next.z) + state.position.z;

                    if (GetStepUpHeight() + state.stepStartZ < top) {
                        double allowed = double(GetStepUpHeight()) + state.stepStartZ - state.position.z;
                        remaining = double(allowed) / float(next.z) * remaining;

                        if (fnotequal(allowed, next.z)) {
                            moved = 1;
                        }

                        if (remaining < 0.001f) {
                            goto fall;
                        }
                    }
                }
            }

        next:
            if (!DistanceToMove(moveDir, remaining, &facetIndex, planes, &planeCount, &moveDist)) {
                return elapsed;
            }
        }

    finish:
        if (cleared) {
            goto fall;
        }

        if (facetIndex >= facets.size()) {
            if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
                return elapsed;
            }

            goto fall;
        }

        if (moved) {
            UpdateAnchors(0);
        }

        SetTransport(transport);
        return elapsed;

    fall:
        StartFalling(0.0f);
        return elapsed;
    }

    // 0x00986f00
    float CalcFallDistance(float time, int32_t safeFall, float velocity) const {
        float terminal = safeFall ? s_safeFallTerminalVelocity : s_terminalVelocity;

        if (velocity > terminal) {
            velocity = terminal;
        }

        if (19.291105f * time + velocity > terminal) {
            float accelTime = (terminal - velocity) * 0.051837362f;
            return terminal * (time - accelTime) + (velocity + 9.6455526f * accelTime) * accelTime;
        }

        return time * (velocity + time * 9.6455526f);
    }

    // 0x00988220
    float TimeToFallDistance(float distance, int32_t safeFall) const {
        float terminal = safeFall ? s_safeFallTerminalVelocity : s_terminalVelocity;
        float accelTime = 0.051837362f * terminal;
        float accelDistance = accelTime * terminal * 0.5f;

        if (distance < accelDistance) {
            if (0.0f < distance) {
                return sqrt(distance * 0.103674725f);
            }

            return 0.0f;
        }

        return (distance - accelDistance) / terminal + accelTime;
    }

    // 0x009880c0
    void CalcCurrentSpeed(int32_t force) {
        if (!(state.flags & MOVEMENTFLAG_FALLING) || force) {
            state.currentSpeed = GetBaseSpeed(force);
        }
    }

    // 0x00987ef0
    void CalcDirection() {
        float facing = state.anchorFacing;
        C2Vector dir2D{0};
        dir2D.x = cos(facing);
        dir2D.y = sin(facing);
        state.direction2D = dir2D;

        if ((state.flags & (MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING)) && fabs(state.anchorPitch) >= 0.00000095367432f) {
            float pitch = state.anchorPitch;
            state.pitchCos = cos(pitch);
            state.pitchSin = sin(pitch);
            state.direction.x = state.pitchCos * dir2D.x;
            state.direction.y = state.pitchCos * dir2D.y;
            state.direction.z = state.pitchSin;
        } else {
            state.direction.x = state.direction2D.x;
            state.direction.y = state.direction2D.y;
            state.pitchSin = 0.0f;
            state.direction.z = 0.0f;
            state.pitchCos = 1.0f;
        }
    }

    // 0x00987ef0
    void CalcDirection(int32_t force) {
        if ((state.flags & MOVEMENTFLAG_FALLING) && !force) {
            return;
        }

        CalcDirection();
        uint32_t flags = state.flags;

        if (flags & (MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD)) {
            if (flags & (MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT)) {
                float forward2DX = state.direction2D.x;
                float forward2DY = state.direction2D.y;
                float forwardX = state.direction.x;
                float forwardY = state.direction.y;
                float forwardZ = state.direction.z;

                if (flags & MOVEMENTFLAG_BACKWARD) {
                    forward2DX = -forward2DX;
                    forward2DY = -forward2DY;
                    forwardX = -forwardX;
                    forwardY = -forwardY;
                    forwardZ = -forwardZ;
                }

                float swap = state.direction2D.x;
                state.direction2D.x = state.direction2D.y;
                state.direction2D.y = swap;

                if (state.flags & MOVEMENTFLAG_STRAFE_LEFT) {
                    state.direction2D.x = -state.direction2D.x;
                } else {
                    state.direction2D.y = -state.direction2D.y;
                }

                state.direction.x = state.direction2D.x + forwardX;
                state.direction.y = state.direction2D.y + forwardY;
                state.direction.z = forwardZ;
                state.direction2D.x = state.direction2D.x + forward2DX;
                state.direction2D.y = state.direction2D.y + forward2DY;
                state.direction.x = state.direction.x * 0.70710677f;
                state.direction.y = state.direction.y * 0.70710677f;
                state.direction.z = state.direction.z * 0.70710677f;
                state.direction2D.x = state.direction2D.x * 0.70710677f;
                state.direction2D.y = 0.70710677f * state.direction2D.y;
                return;
            }
        } else if (flags & (MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT)) {
            float swap = state.direction2D.x;
            state.direction2D.x = state.direction2D.y;
            state.direction2D.y = swap;

            if (state.flags & MOVEMENTFLAG_STRAFE_LEFT) {
                state.direction2D.x = -state.direction2D.x;
            } else {
                state.direction2D.y = -state.direction2D.y;
            }

            state.direction.x = state.direction2D.x;
            state.direction.y = state.direction2D.y;
            state.direction.z = 0.0f;
            return;
        }

        if (flags & MOVEMENTFLAG_BACKWARD) {
            state.direction = C3Vector(-state.direction.x, -state.direction.y, -state.direction.z);
            float x = -state.direction2D.x;
            float y = -state.direction2D.y;
            state.direction2D.x = x;
            state.direction2D.y = y;
        }
    }

    // 0x00988280
    float CalcTimeFallen(float distance, int32_t upward) const {
        float terminal = (state.flags & MOVEMENTFLAG_SAFE_FALL) ? s_safeFallTerminalVelocity : s_terminalVelocity;
        float velocity = state.fallVelocity;

        if (velocity > terminal) {
            velocity = terminal;
        }

        if (fabs(velocity) < 0.00000023841858f) {
            return TimeToFallDistance(distance, state.flags & MOVEMENTFLAG_SAFE_FALL);
        }

        float discriminant = velocity * velocity + 38.582211f * distance;
        float root = 0.0f;

        if (discriminant > 0.0f) {
            root = sqrt(discriminant);
        }

        float timeUp = (-velocity - root) * 0.051837362f;
        float timeDown = (root - velocity) * 0.051837362f;
        float accelTime = 0.051837362f * (terminal - velocity);
        float result;

        if (timeDown <= accelTime) {
            result = timeDown;
        } else {
            result = (distance - (velocity + accelTime * 9.6455526f) * accelTime) / terminal + accelTime;
        }

        if (upward) {
            result = 0.0f;

            if (timeUp >= 0.0f) {
                return timeUp;
            }
        }

        return result;
    }

    // 0x00987570
    float GetBaseSpeed(int32_t walk) const {
        uint32_t flags = state.flags;

        if (!(flags & (MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT | MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING))) {
            return 0.0f;
        }

        if (flags & MOVEMENTFLAG_FLYING) {
            if ((flags & MOVEMENTFLAG_BACKWARD) && state.flightSpeed >= state.flightBackSpeed) {
                return state.flightBackSpeed;
            }

            return state.flightSpeed;
        }

        if (flags & MOVEMENTFLAG_SWIMMING) {
            if ((flags & MOVEMENTFLAG_BACKWARD) && state.swimSpeed >= state.swimBackSpeed) {
                return state.swimBackSpeed;
            }

            return state.swimSpeed;
        }

        if ((flags & MOVEMENTFLAG_WALKING) || walk) {
            if (state.runSpeed > state.walkSpeed) {
                return state.walkSpeed;
            }
        } else if ((flags & MOVEMENTFLAG_BACKWARD) && state.runSpeed >= state.runBackSpeed) {
            return state.runBackSpeed;
        }

        return state.runSpeed;
    }

    // 0x009870d0
    float RelDistanceFallen(uint32_t time, float z) const {
        uint32_t flags = state.flags;
        float velocity = state.fallVelocity;

        if (flags & MOVEMENTFLAG_FALLING) {
            return CalcFallDistance(time * 0.001f, flags & MOVEMENTFLAG_SAFE_FALL, velocity) + z - state.fallStartZ;
        }

        return CalcFallDistance(time * 0.001f, flags & MOVEMENTFLAG_SAFE_FALL, velocity);
    }

    // 0x00988490
    void StopFalling() {
        uint32_t flags = state.flags;
        uint32_t falling = flags & MOVEMENTFLAG_FALLING;

        if (falling) {
            state.flags = flags & ~(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        }

        if (state.flags & MOVEMENTFLAG_PENDING_ROOT) {
            state.flags = (state.flags & 0xFF203700) | MOVEMENTFLAG_ROOT;
        } else if (!falling) {
            return;
        }

        UpdateAnchors(0);
        CalcCurrentSpeed(0);
    }

    // 0x00986e80
    float TimeToJumpPeak() const {
        if (state.flags & MOVEMENTFLAG_FALLING) {
            return state.fallVelocity * -0.051837362f;
        }

        return 0.0f;
    }

    // 0x009881d0
    void UpdateAnchors(int32_t force) {
        state.anchorFacing = state.facing;
        state.anchorPosition = state.position;
        state.anchorPitch = state.pitch;
        state.anchorElapsedMs = 0;
        CalcDirection(force);
        
    }

    // 0x0075cf80
    void UpdateFallingFar() {
        uint32_t flags = state.flags;

        if (flags & (MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_SAFE_FALL)) {
            return;
        }

        if ((flags & MOVEMENTFLAG_FALLING) && 0.0f != state.fallVelocity) {
            if (state.fallStartZ - 0.11111111f >= state.position.z) {
                state.flags = flags | MOVEMENTFLAG_FALLING_FAR;
            }
        } else if (state.fallTimeMs >= 500) {
            state.flags = flags | MOVEMENTFLAG_FALLING_FAR;
        }
    }

    // 0x009870d0
    float RelDistanceFallen(uint32_t time) const {
        return RelDistanceFallen(time, state.position.z);
    }

    // 0x00988370
    int32_t StartFalling(float velocity) {
        if (state.flags & (MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_ROOT)) {
            return 0;
        }


        UpdateAnchors(0);
        state.flags = (state.flags & ~(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_SPLINE_ELEVATION)) | MOVEMENTFLAG_FALLING;

        if (!(state.flags2 & MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING)) {
            state.flags &= ~(MOVEMENTFLAG_PITCH_UP | MOVEMENTFLAG_PITCH_DOWN);
        }

        state.fallTimeMs = 0;
        state.fallStartZ = state.position.z;
        state.fallVelocity = velocity;
        return 1;
    }
};
} // namespace

SurfaceMovementResult traceMovementSurface(std::span<const MovementFacet> facets,
    SurfaceMovementState& state, const SurfaceMovementShape& shape,
    const glm::vec2& direction, float distance, uint32_t elapsedMs) {
    SurfaceTraversal traversal{facets, state, shape.radius, shape.height,
                               shape.stepHeight, shape.clientControlled, std::nullopt};
    const auto used = traversal.TraceSurface(elapsedMs, distance, direction);
    return {used, traversal.transportRequest};
}
} // namespace wowee::collision
