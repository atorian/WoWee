#version 450

// The ground's draws made on the GPU (TerrainRenderer, GPU-driven stage 3),
// one thread per row of the chunk table, in two passes over it.
//
// The first decides for each chunk whether the view and the water's
// reflection can see it, and the level it is drawn at: the coarsest whose
// error stays under a pixel from its nearest point.
//
// The second makes the draws of each chunk the view or the reflection sees:
// a record of what the shaders read of it - its row, how its surface is
// read, the lights that reach it - and a draw command for its level and one
// for each edge whose neighbour is missing or drawn at another level, where
// a skirt hides the crack. The commands go to the group of the chunk's
// geometry (a page of the vertex and index buffers it shares) and to the
// pipeline it needs: the one without the distance discards when it ends
// short of them.
//
// The rules are TerrainRenderer's from before, the same arithmetic in the
// same order: terrainLodFor, terrainSurfaceSourceFor, pickLocalLights,
// terrainChunkShortOfDiscards, Frustum::intersectsSphere and
// WaterReflectionView::canSee.

layout(local_size_x = 64) in;

// TerrainCullRowGPU: what the cull reads of a chunk.
struct CullRow {
    vec4 sphere;        // xyz centre, w radius
    vec4 lodErrors;     // by level, in yards; level 0 is the full mesh
    uvec4 neighbours;   // rows across its edges y-1, y+1, x-1, x+1; ~0 none
    uvec4 geometry;     // x group, y first index, z base vertex (int), w offered levels
    uvec4 surface;      // x cell in its tile's atlas, y flags, z steepest up-z (float bits)
    uvec2 lods[4];      // first, count of each level's surface
    uvec2 skirts[16];   // [level * 4 + edge]
};
const uint kRowValid = 1u;
const uint kRowBaked = 2u;

layout(std430, set = 0, binding = 0) readonly buffer CullRows { CullRow rows[]; };

layout(std140, set = 0, binding = 1) uniform Frame {
    vec4 viewPlanes[6];
    vec4 mirrorPlanes[6];
    vec4 camera;        // xyz position, w focal length in pixels
    vec4 limits;        // x reach, y where the discards start, z, w the bake band
    vec4 mirror;        // x 1 when the reflection is drawn, y the water's height,
                        // z the relief strength, w 1 when the view is frustum-culled
    uvec4 counts;       // x rows, y lights, z 1 with levels of detail, w 1 with the bake
    uvec4 groups;       // x groups
} frame;

// Per row: the level it is drawn at, -1 when neither sees it; bit 8 the
// view, bit 9 the reflection.
layout(std430, set = 0, binding = 2) buffer Levels { int levels[]; };

// TerrainDrawRecordGPU: what the ground's shaders read of one chunk's draws.
struct DrawRecord {
    uvec4 row;          // x the chunk's row
    vec4 relief;
    vec4 baked;
    uvec4 lights;
};
layout(std430, set = 0, binding = 3) writeonly buffer Records { DrawRecord records[]; };

struct DrawCommand {    // VkDrawIndexedIndirectCommand
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout(std430, set = 0, binding = 4) writeonly buffer Commands { DrawCommand commands[]; };

// Per group and list (view without discards, view with, reflection
// without, reflection with): where its commands start; and how many it has.
layout(std430, set = 0, binding = 5) readonly buffer Regions { uint regionStart[]; };
layout(std430, set = 0, binding = 6) buffer Counts { uint counts[]; };

// What was drawn, for the frame's statistics.
layout(std430, set = 0, binding = 7) buffer Stats {
    uint drawn;
    uint culled;
    uint baked;
    uint crossfade;
    uint furthestSq;    // float bits; distances are never negative
    uint triangles;
    uint draws;
    uint recordCount;
} stats;

layout(std430, set = 0, binding = 8) readonly buffer LocalLightBuffer { vec4 localLightData[]; };

layout(push_constant) uniform Pass { uint second; } pass;

const float kLodMaxErrorPixels = 1.0;

#include "local_lights_pick.glsli"

bool insidePlanes(vec4 planes[6], vec3 centre, float radius) {
    for (int i = 0; i < 6; ++i) {
        precise float distance = dot(planes[i].xyz, centre) + planes[i].w;
        if (distance < -radius) return false;
    }
    return true;
}

int lodFor(CullRow row, float nearest) {
    if (frame.counts.z == 0u) return 0;
    precise float d = max(nearest, 1.0);
    int chosen = 0;
    for (int level = 1; level < 4; ++level) {
        if ((row.geometry.w & (1u << uint(level))) == 0u) break;
        precise float pixels = row.lodErrors[level] * frame.camera.w / d;
        if (pixels > kLodMaxErrorPixels) break;
        chosen = level;
    }
    return chosen;
}
void emit(uint list, CullRow row, uint level, uint record, uint group) {
    uint base = regionStart[group * 4u + list];
    const uvec2 surface = row.lods[level];
    uint ranges = surface.y > 0u ? 1u : 0u;
    uvec2 parts[5];
    parts[0] = surface;
    for (uint edge = 0u; edge < 4u; ++edge) {
        uint neighbour = row.neighbours[edge];
        bool skirt = neighbour == 0xFFFFFFFFu || levels[neighbour] < 0 || (levels[neighbour] & 0xFF) != int(level);
        uvec2 range = row.skirts[level * 4u + edge];
        if (skirt && range.y > 0u) parts[ranges++] = range;
    }
    if (ranges == 0u) return;
    uint at = atomicAdd(counts[group * 4u + list], ranges);
    for (uint k = 0u; k < ranges; ++k) {
        DrawCommand c;
        c.indexCount = parts[k].y;
        c.instanceCount = 1u;
        c.firstIndex = row.geometry.y + parts[k].x;
        c.vertexOffset = int(row.geometry.z);
        c.firstInstance = record;
        commands[base + at + k] = c;
        if (list < 2u) atomicAdd(stats.triangles, parts[k].y / 3u);
    }
    if (list < 2u) atomicAdd(stats.draws, ranges);
}

void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= frame.counts.x) return;
    CullRow row = rows[index];
    if ((row.surface.y & kRowValid) == 0u) {
        if (pass.second == 0u) levels[index] = -1;
        return;
    }
    vec3 centre = row.sphere.xyz;
    float radius = row.sphere.w;
    vec3 camPos = frame.camera.xyz;

    if (pass.second == 0u) {
        vec2 step = vec2(centre - camPos);
        precise float distanceSq = dot(step, step);
        float limit = frame.limits.x + radius;
        bool inReach = distanceSq <= limit * limit;
        bool view = inReach && (frame.mirror.w == 0.0 || insidePlanes(frame.viewPlanes, centre, radius));
        bool mirror = inReach && frame.mirror.x != 0.0 && centre.z + radius >= frame.mirror.y &&
                      insidePlanes(frame.mirrorPlanes, centre, radius);
        if (!view) atomicAdd(stats.culled, 1u);
        if (!view && !mirror) {
            levels[index] = -1;
            return;
        }
        precise float nearest = length(centre - camPos) - radius;
        int level = lodFor(row, nearest);
        levels[index] = level | (view ? 0x100 : 0) | (mirror ? 0x200 : 0);
        if (view) {
            atomicAdd(stats.drawn, 1u);
            atomicMax(stats.furthestSq, floatBitsToUint(distanceSq));
        }
        return;
    }

    int seen = levels[index];
    if (seen < 0) return;
    uint level = uint(seen & 0xFF);
    bool view = (seen & 0x100) != 0;
    bool mirror = (seen & 0x200) != 0;

    precise float distance = length(centre - camPos);
    uint source = 0u;  // TerrainSurfaceSource: 0 layers, 1 crossfade, 2 baked
    if (frame.counts.w != 0u && (row.surface.y & kRowBaked) != 0u) {
        float upZ = clamp(uintBitsToFloat(row.surface.z), 0.0, 1.0);
        precise float nearest = distance - radius;
        precise float farthest = distance + radius;
        if (nearest * upZ >= frame.limits.w) source = 2u;
        else if (farthest <= frame.limits.z) source = 0u;
        else source = 1u;
    }
    if (view && source == 2u) atomicAdd(stats.baked, 1u);
    if (view && source == 1u) atomicAdd(stats.crossfade, 1u);

    uint record = atomicAdd(stats.recordCount, 1u);
    DrawRecord r;
    r.row = uvec4(index, 0u, 0u, 0u);
    r.relief = vec4(frame.mirror.z, 0.0, 0.0, 0.0);
    r.baked = vec4(float(source), frame.limits.z, frame.limits.w, source == 0u ? 0.0 : float(row.surface.x));
    r.lights = pickLocalLights(centre, radius, frame.counts.y);
    records[record] = r;

    precise float horizontal = length(vec2(centre - camPos));
    bool shortOfDiscards = horizontal + radius + 1.0 < frame.limits.y;
    uint group = row.geometry.x;
    if (view) emit(shortOfDiscards ? 0u : 1u, row, level, record, group);
    if (mirror) emit(shortOfDiscards ? 2u : 3u, row, level, record, group);
}
