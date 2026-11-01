#version 450

// GPU culling for M2 instances: one thread per slot of the scene's cull
// table (M2Renderer, InstanceSlots). The table lives on the GPU and changes
// only where an instance did; each frame every slot is tested against the
// view's frustum and the instance's own draw distance, and the kept ones are
// appended to a list the CPU reads back: (slot, id) pairs.

layout(local_size_x = 64) in;

// One slot. Its flags are the instance's cull record flags (M2CullGrid);
// a free slot is all zero, and not valid.
struct CullRow {
    vec4  sphere;           // xyz = cull centre, w = padded radius
    float maxDistSqFactor;  // the instance's share of the draw distance, squared
    uint  flags;
    uint  id;
    uint  _pad0;
};

layout(std140, set = 0, binding = 0) uniform CullUniforms {
    vec4  frustumPlanes[6];       // xyz = normal, w = distance
    vec4  cameraPos;              // xyz = camera position, w = maxPossibleDistSq
    uint  slotCount;
    float baseMaxDistSq;          // the adaptive doodad range, squared
    float viewDistance;           // nothing is drawn past this
    float groundDetailMaxDistance;
    float gameObjectMinDistance;
    float skyBirdMaxDistance;
    uint  _pad1;
    uint  _pad2;
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

void main() {
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= slotCount) return;

    CullRow row = rows[slot];
    uint f = row.flags;
    if ((f & kValid) == 0u || (f & (kInvisibleTrap | kHidden | kFinishing)) != 0u) return;

    vec3 toCam = row.sphere.xyz - cameraPos.xyz;
    float distSq = dot(toCam, toCam);
    if (distSq > cameraPos.w) return;
    if (distSq > instanceMaxDistSq(row)) return;

    float radius = row.sphere.w;
    if (radius > 0.0) {
        for (int i = 0; i < 6; i++) {
            float d = dot(frustumPlanes[i].xyz, row.sphere.xyz) + frustumPlanes[i].w;
            if (d < -radius) return;
        }
    }

    uint at = atomicAdd(visibleCount, 1u);
    visible[2u * at] = slot;
    visible[2u * at + 1u] = row.id;
}
