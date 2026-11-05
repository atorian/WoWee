#version 450

// The M2 view's draws, written on the GPU (M2Renderer::prepareViewDraws).
// A workgroup takes one group of the frame - the visible instances of one
// model at one level of detail, whose base records the CPU wrote in a row -
// and writes a draw command for each batch the model draws at that level,
// at the place the CPU gave the batch's bucket for this group. A batch whose
// instances differ per batch - a texture animation, the batch's own opacity,
// the sky's opacity of the hour - gets records of its own: the base record
// with its UV transforms sampled at the instance's clock and its alpha.
//
// The model's draw list and its texture animations are blobs in the
// SkeletonPool's pages (m2_draw_pass.hpp, skeleton_pool.hpp say the layout).

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) readonly buffer Page {
    uint words[];
} pages[64];

#include "m2_track.glsli"

// A group: one model at one level of detail (M2DrawGroupGPU).
struct Group {
    uint listPage;       // the model's draw list
    uint listBlob;
    uint lod;
    uint firstRecord;    // its instances' base records, in a row
    uint instanceCount;
    uint ownFirst;       // where its batches' own records start
    uint offsetsFirst;   // its buckets' first commands, in offsets[]
    uint uvPage;         // the model's texture animations; ~0 for none
    uint uvBlob;
    uint opacityFirst;   // the sky's opacity of each of its own records, ~0 for none
    uint pad0;
    uint pad1;
};
layout(std430, set = 1, binding = 0) readonly buffer Groups {
    Group groups[];
};
layout(std430, set = 1, binding = 1) readonly buffer Offsets {
    uint offsets[];
};
// Each base record's clock: animation time and global time in ms, the
// sequence index, unused.
layout(std430, set = 1, binding = 2) readonly buffer Clocks {
    vec4 clocks[];
};
layout(std430, set = 1, binding = 3) readonly buffer SkyOpacity {
    float skyOpacity[];
};

// m2.vert's InstanceData.
struct InstanceData {
    vec4 uvRow0;
    vec4 uvRow1;
    vec4 uv2Row0;
    vec4 uv2Row1;
    float fadeAlpha;
    int useBones;
    int boneBase;
    int boneCount;
    float highlight;
    int flags;
    uint slot;
    float pad0;
    uvec4 localLights;
};
layout(std430, set = 1, binding = 4) buffer Instances {
    InstanceData instanceData[];
};

struct Command {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout(std430, set = 1, binding = 5) writeonly buffer Commands {
    Command commands[];
};
layout(std430, set = 1, binding = 6) writeonly buffer DrawMaterials {
    uint drawMaterials[];
};

// The draw list blob (m2_draw_pass.hpp).
const uint kLodHeaderWords = 4u;    // per level: entries, entry count, own list, own count
const uint kEntryWords = 12u;
const uint kEntryIndexCount = 0u;
const uint kEntryFirstIndex = 1u;
const uint kEntryBaseVertex = 2u;
const uint kEntryMaterialRow = 3u;
const uint kEntryBucket = 4u;
const uint kEntryOrdinal = 5u;
const uint kEntryUvAnim = 6u;       // and 7: the two stages' transforms, ~0 for none
const uint kEntryOpacity = 8u;
const uint kEntryFlags = 9u;
const uint kEntryOwnOrdinal = 10u;
const uint kFlagOwnRecords = 1u;

#include "m2_uv_transform.glsli"

void main() {
    const Group g = groups[gl_WorkGroupID.x];
    const uint lane = gl_LocalInvocationID.x;
    pageIndex = g.listPage;
    blobBase = g.listBlob;
    const uint header = g.lod * kLodHeaderWords;
    const uint entries = word(header);
    const uint entryCount = word(header + 1u);
    const uint ownList = word(header + 2u);
    const uint ownCount = word(header + 3u);

    // A command a batch.
    for (uint e = lane; e < entryCount; e += 64u) {
        const uint at = entries + e * kEntryWords;
        const uint command = offsets[g.offsetsFirst + word(at + kEntryBucket)] + word(at + kEntryOrdinal);
        const bool own = (word(at + kEntryFlags) & kFlagOwnRecords) != 0u;
        Command c;
        c.indexCount = word(at + kEntryIndexCount);
        c.instanceCount = g.instanceCount;
        c.firstIndex = word(at + kEntryFirstIndex);
        c.vertexOffset = int(word(at + kEntryBaseVertex));
        c.firstInstance = own ? g.ownFirst + word(at + kEntryOwnOrdinal) * g.instanceCount : g.firstRecord;
        commands[command] = c;
        drawMaterials[command] = word(at + kEntryMaterialRow);
    }

    // Own records: each such batch's run of the group's instances.
    const uint total = ownCount * g.instanceCount;
    for (uint k = lane; k < total; k += 64u) {
        const uint ownOrdinal = k / g.instanceCount;
        const uint instance = k - ownOrdinal * g.instanceCount;
        pageIndex = g.listPage;
        blobBase = g.listBlob;
        const uint entry = word(ownList + ownOrdinal);
        const uint at = entries + entry * kEntryWords;
        const uint uv0 = word(at + kEntryUvAnim);
        const uint uv1 = word(at + kEntryUvAnim + 1u);
        float alpha = uintBitsToFloat(word(at + kEntryOpacity));
        if (g.opacityFirst != kNone) alpha *= skyOpacity[g.opacityFirst + k];

        const uint base = g.firstRecord + instance;
        InstanceData record = instanceData[base];
        const vec4 clock = clocks[base];
        const int sequence = floatBitsToInt(clock.z);
        record.uvRow0 = vec4(1.0, 0.0, 0.0, 0.0);
        record.uvRow1 = vec4(0.0, 1.0, 0.0, 0.0);
        record.uv2Row0 = record.uvRow0;
        record.uv2Row1 = record.uvRow1;
        if (g.uvPage != kNone) {
            pageIndex = g.uvPage;
            blobBase = g.uvBlob;
            uvTransform(uv0, sequence, clock.x, clock.y, record.uvRow0, record.uvRow1);
            uvTransform(uv1, sequence, clock.x, clock.y, record.uv2Row0, record.uv2Row1);
        }
        record.fadeAlpha = alpha;
        instanceData[g.ownFirst + k] = record;
    }
}
