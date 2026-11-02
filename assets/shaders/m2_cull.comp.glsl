#version 450

// GPU culling for M2 instances: one thread per slot of the scene's cull
// table (M2Renderer, InstanceSlots). The table lives on the GPU and changes
// only where an instance did; each frame every slot is tested against the
// guard of the view (cull_guard.hpp: its sides widened and pushed out, its
// ranges stretched) and the kept ones are appended to a list the CPU reads
// back: (slot, id) pairs. The CPU reads it a frame or two later, and tests
// what it kept against the view of its own frame.

layout(local_size_x = 64) in;

// One slot. Its flags are the instance's cull record flags (M2CullGrid);
// a free slot is all zero, and not valid.
struct CullRow {
    vec4  sphere;           // xyz = cull centre, w = padded radius
    float maxDistSqFactor;  // the instance's share of the draw distance, squared
    uint  flags;
    uint  id;
    float centreOffset;     // how far the cull centre lies from its position,
                            // which the CPU measures its distance from
};

layout(std140, set = 0, binding = 0) uniform CullUniforms {
    vec4  guardPlanes[4];         // the guard's sides, inward: xyz = normal, w = distance
    vec4  cameraPos;              // xyz = camera position, w = maxPossibleDistSq
    uint  slotCount;
    float baseMaxDistSq;          // the adaptive doodad range, squared
    float viewDistance;           // nothing is drawn past this
    float groundDetailMaxDistance;
    float gameObjectMinDistance;
    float skyBirdMaxDistance;
    float guardDistance;          // how far the camera may move (kCullGuardDistance)
    float rangeSlack;             // how much a range may grow (kCullGuardRangeSlack)
};

layout(std430, set = 0, binding = 1) readonly buffer CullTable {
    CullRow rows[];
};

layout(std430, set = 0, binding = 2) buffer CullVisible {
    uint visibleCount;
    uint visible[];  // slot, id, slot, id, ...
};

const uint kValid = 1u;
const uint kInvisibleTrap = 4u;
const uint kGroundDetail = 8u;
const uint kAnimatedSkyBird = 16u;
const uint kGameObject = 32u;
const uint kHidden = 64u;
const uint kFinishing = 128u;

// m2InstanceMaxDistSq (m2_view_distance.hpp), and a sky bird's own range.
float instanceMaxDistSq(CullRow row) {
    float maxDistSq = baseMaxDistSq * row.maxDistSqFactor;
    if ((row.flags & kGameObject) != 0u)
        maxDistSq = max(maxDistSq, gameObjectMinDistance * gameObjectMinDistance);
    if ((row.flags & kGroundDetail) != 0u && groundDetailMaxDistance > 0.0)
        maxDistSq = min(maxDistSq, groundDetailMaxDistance * groundDetailMaxDistance);
    maxDistSq = min(maxDistSq, viewDistance * viewDistance);
    if ((row.flags & kAnimatedSkyBird) != 0u)
        maxDistSq = min(maxDistSq, skyBirdMaxDistance * skyBirdMaxDistance);
    return maxDistSq;
}

// A range as the guard stretches it.
float reach(float maxDistSq) {
    return sqrt(maxDistSq) * rangeSlack + guardDistance;
}

void main() {
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= slotCount) return;

    CullRow row = rows[slot];
    uint f = row.flags;
    if ((f & kValid) == 0u || (f & (kInvisibleTrap | kHidden | kFinishing)) != 0u) return;

    // No nearer than this can its position be to the camera.
    float nearest = length(row.sphere.xyz - cameraPos.xyz) - row.centreOffset;
    if (nearest > reach(cameraPos.w)) return;
    if (nearest > reach(instanceMaxDistSq(row))) return;

    float radius = row.sphere.w;
    if (radius > 0.0) {
        for (int i = 0; i < 4; i++) {
            float d = dot(guardPlanes[i].xyz, row.sphere.xyz) + guardPlanes[i].w;
            if (d < -radius) return;
        }
    }

    uint at = atomicAdd(visibleCount, 1u);
    visible[2u * at] = slot;
    visible[2u * at + 1u] = row.id;
}
