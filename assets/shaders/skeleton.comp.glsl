#version 450

// Every animated model's bones, posed on the GPU - the only place they are
// made (SkeletonPool, CharacterRenderer). A workgroup poses one request: its
// bones level by level from the root, each bone's tracks sampled at the
// request's time as m2_track_sampler.hpp sampled them, composed onto its
// parent's matrix, turned to the view where it is a billboard bone. A request
// that rides another's bone - a weapon in a hand, an enchant on a weapon -
// takes its model matrix from that bone first; it is dispatched in a later
// level than the one it rides.
//
// The skeleton is a blob of 32-bit words in one of the pool's pages
// (skeleton_pool.hpp says the layout); keys were cleaned when packed: no
// value that is not finite, every rotation a unit quaternion.

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) readonly buffer Page {
    uint words[];
} pages[64];

struct Request {
    uint page;           // the skeleton's page and first word
    uint blob;
    uint outBase;        // its first bone in bones[]
    uint flags;          // SkeletonRequestGPU's: bit 0 turn billboards to the view, bit 1 arms on
                         // their own sequences, bit 2 no skeleton (placed, not posed), bit 3 a
                         // scale under a thousandth is one
    int sequence;        // the body's sequence index, and its time in ms
    float time;
    float globalTime;    // ms, for global sequences
    float torsoYaw;      // radians about Z on the lower spine
    ivec2 armSequence;   // left, right arm: their sequence, or -1 to follow the body
    vec2 armTime;        // and their time in it
    int parent;          // the request it rides, or -1 for one placed by its own matrix
    uint parentBone;     // the parent's bone it rides, ~0 for the parent's model itself
    uint pad0;
    uint pad1;
    mat4 placement;      // its model matrix, or for a rider what follows the parent's bone
    vec4 shift;          // xyz: moved by this in the world after it is placed
};

layout(set = 0, binding = 1) readonly buffer Requests {
    Request requests[];
};
// Coherent: a level reads the bones the level before it wrote.
layout(set = 0, binding = 2) coherent buffer Bones {
    mat4 bones[];
};
// Each request's model matrix as posed: its own, or its parent's bone's.
layout(set = 0, binding = 3) buffer Models {
    mat4 models[];
};

layout(push_constant) uniform Push {
    mat4 view;           // billboard bones turn against it
    uint firstRequest;   // this dispatch's level
    uint pad0, pad1, pad2;
} push;

#include "m2_track.glsli"

// Skeleton blob header and bone record (skeleton_pool.cpp).
const uint kHeaderBoneCount = 0u;
const uint kHeaderMaxDepth = 1u;
const uint kHeaderBones = 2u;
const uint kBoneWords = 16u;
const int kKeyBoneSpineLow = 4;

// A billboard bone, as the client turns it (CM2Model::Animate, WoW.exe 3.3.5a
// 0x82f0f0, dispatch 0x82ff3a; render spec section 3.5): in VIEW space, after
// the bone's whole parent chain and the model's placement, against the view's
// fixed axis rather than toward the eye point - every card of a model turns
// alike, as on a screen. The bone's axes are rebuilt per its flag (0x8
// spherical, 0x10/0x20/0x40 keeping X/Y/Z), each keeping its length, and the
// pivot stays where it was. A card faces +X, Y left, Z up. The client's view
// space is left-handed; this one is right-handed, so the cross products run
// the other way round from the disassembly and +X comes out toward the
// viewer. A spherical billboard of a bone with its own tracks (0x280) still
// turns by them.
//
// A vector too short to say a direction is left as it is, zero staying zero
// and the card collapsing - the client has no fallback.
vec3 billboardNormalize(vec3 v) {
    const float len2 = dot(v, v);
    return len2 > 2.384e-7 ? v * inversesqrt(len2) : v;
}

mat4 billboardBone(uint flags, mat4 boneView, mat3 local, vec3 pivot) {
    const vec3 s = vec3(length(boneView[0].xyz), length(boneView[1].xyz), length(boneView[2].xyz));
    const vec3 pivotView = (boneView * vec4(pivot, 1.0)).xyz;
    vec3 c0 = boneView[0].xyz, c1 = boneView[1].xyz, c2 = boneView[2].xyz;
    const uint kinds = flags & 0x78u;
    if (bitCount(kinds) > 1) {
        // Two kinds at once rebuild nothing and only rescale, which squares
        // the axes' lengths - as the client does.
    } else if (kinds == 0x10u) {
        c0 = billboardNormalize(c0);
        c1 = billboardNormalize(vec3(c0.y, -c0.x, 0.0));
        c2 = cross(c0, c1);
    } else if (kinds == 0x20u) {
        c1 = billboardNormalize(c1);
        c0 = billboardNormalize(vec3(-c1.y, c1.x, 0.0));
        c2 = cross(c0, c1);
    } else if (kinds == 0x40u) {
        c2 = billboardNormalize(c2);
        c1 = billboardNormalize(vec3(c2.y, -c2.x, 0.0));
        c0 = cross(c1, c2);
    } else if (kinds == 0x8u) {
        if ((flags & 0x280u) != 0u) {
            c0 = billboardNormalize(vec3(local[0].y, local[0].z, local[0].x));
            c1 = billboardNormalize(vec3(local[1].y, local[1].z, local[1].x));
            c2 = billboardNormalize(vec3(local[2].y, local[2].z, local[2].x));
        } else {
            c0 = vec3(0.0, 0.0, 1.0);
            c1 = vec3(1.0, 0.0, 0.0);
            c2 = vec3(0.0, 1.0, 0.0);
        }
    } else {
        return boneView;
    }
    c0 *= s.x;
    c1 *= s.y;
    c2 *= s.z;
    return mat4(vec4(c0, 0.0), vec4(c1, 0.0), vec4(c2, 0.0),
                vec4(pivotView - (pivot.x * c0 + pivot.y * c1 + pivot.z * c2), 1.0));
}

void main() {
    const uint requestIndex = push.firstRequest + gl_WorkGroupID.x;
    const Request request = requests[requestIndex];
    pageIndex = request.page;
    blobBase = request.blob;

    // Where it stands: its own matrix, or on its parent's bone.
    mat4 model = request.placement;
    if (request.parent >= 0) {
        const Request parent = requests[uint(request.parent)];
        const mat4 bone = request.parentBone == kNone ? mat4(1.0) : bones[parent.outBase + request.parentBone];
        model = models[uint(request.parent)] * bone * request.placement;
    }
    model[3].xyz += request.shift.xyz;
    if (gl_LocalInvocationIndex == 0u) models[requestIndex] = model;
    // A model without a skeleton in the pool is placed and drawn unposed.
    if ((request.flags & 4u) != 0u) return;

    const uint boneCount = word(kHeaderBoneCount);
    const uint maxDepth = word(kHeaderMaxDepth);
    const uint boneRecords = word(kHeaderBones);
    const bool turnBillboards = (request.flags & 1u) != 0u;
    const bool armsOwn = (request.flags & 2u) != 0u;
    const mat4 viewModel = push.view * model;
    const mat4 invViewModel = inverse(viewModel);

    for (uint depth = 0u; depth <= maxDepth; ++depth) {
        for (uint i = gl_LocalInvocationIndex; i < boneCount; i += gl_WorkGroupSize.x) {
            const uint record = boneRecords + i * kBoneWords;
            if (word(record + 7u) != depth) continue;
            const vec3 pivot = vec3(wordf(record), wordf(record + 1u), wordf(record + 2u));
            const int parentBone = int(word(record + 3u));
            const uint flags = word(record + 4u);
            const int keyBoneId = int(word(record + 5u));
            const int arm = int(word(record + 6u));

            int sequence = request.sequence;
            float time = request.time;
            if (armsOwn && arm >= 0 && request.armSequence[arm] >= 0) {
                sequence = request.armSequence[arm];
                time = request.armTime[arm];
            }
            const vec3 translation = sampleVec3(word(record + 8u), sequence, time, request.globalTime, vec3(0.0));
            const vec4 rotation = sampleQuat(word(record + 9u), sequence, time, request.globalTime);
            vec3 scale = sampleVec3(word(record + 10u), sequence, time, request.globalTime, vec3(1.0));
            // The doodads' rule: a scale under a thousandth is one.
            if ((request.flags & 8u) != 0u) scale = mix(scale, vec3(1.0), lessThan(scale, vec3(0.001)));

            // T(pivot) T(translation) R S T(-pivot)
            const mat3 r = quatToMat3(rotation);
            const mat3 rs = mat3(r[0] * scale.x, r[1] * scale.y, r[2] * scale.z);
            mat4 local = mat4(vec4(rs[0], 0.0), vec4(rs[1], 0.0), vec4(rs[2], 0.0),
                              vec4((pivot + translation) - rs * pivot, 1.0));
            if (keyBoneId == kKeyBoneSpineLow && request.torsoYaw != 0.0) {
                const float c = cos(request.torsoYaw), s = sin(request.torsoYaw);
                const mat4 yaw = mat4(vec4(c, s, 0.0, 0.0), vec4(-s, c, 0.0, 0.0), vec4(0.0, 0.0, 1.0, 0.0),
                                      vec4(pivot - mat3(vec3(c, s, 0.0), vec3(-s, c, 0.0), vec3(0.0, 0.0, 1.0)) * pivot,
                                           1.0));
                local = yaw * local;
            }
            mat4 world = parentBone >= 0 && uint(parentBone) < boneCount
                ? bones[request.outBase + uint(parentBone)] * local : local;
            if (turnBillboards && (flags & 0x78u) != 0u)
                world = invViewModel * billboardBone(flags, viewModel * world, mat3(local), pivot);
            bones[request.outBase + i] = world;
        }
        memoryBarrierBuffer();
        barrier();
    }
}
