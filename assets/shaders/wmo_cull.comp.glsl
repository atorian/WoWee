#version 450

// The buildings' draws made on the GPU (WMORenderer, wmo_draws.hpp), one
// thread per group of a placed building.
//
// Every group has a place of its own for each of its draws, kept for as
// long as it is placed: in the colour lists, in the bucket of its geometry
// page and its kind of material; in the shadow maps, in its page's casters.
// A group a pass does not draw leaves its places empty - the commands are
// zeroed before the cull - so a bucket keeps one order from frame to frame
// and is drawn whole by one indirect call.
//
// The colour pass decides for each group whether the view and the water's
// reflection draw it, by WMORenderer's rules from before: the horizontal
// view distance, the nearer group distance when asked for, the camera's
// volume for the view, what the mirror can show of the group and its swung
// cloth for the reflection, the distance-only shells beyond 196 yards, and
// the portal walk's verdict when it runs (the CPU's, by bit). It picks the
// lights that reach a drawn group (local_lights_pick.glsli). A depth pass
// takes every group that reaches its map: its box within the map's reach of
// its centre, shells and antiportals left out.

layout(local_size_x = 64) in;

// WmoInstanceGPU: where a building stands; state.x 1 while it is in the
// world.
struct Instance {
    mat4 model;
    uvec4 state;
};
// WmoGroupGPU: a placed group.
struct Group {
    vec4 worldMin;      // its box in the world, padded; w how far its cloth swings past it
    vec4 worldMax;
    vec4 localCentre;   // its box's centre in its model, for the shells' distance
    uvec4 refs;         // x the instance's slot, y its first range, z its ranges, w flags
    uvec4 slots0;       // its first place in the colour buckets of kinds 0..3
    uvec4 slots1;       // x, y kinds 4 and 5; z its first place among its page's casters; w its page
};
const uint kGroupValid = 1u;
const uint kGroupShell = 2u;
const uint kGroupAntiportal = 4u;

// WmoRangeGPU: one index range of a group's merged batch.
struct Range {
    uint indexCount;
    uint firstIndex;    // in its geometry page
    int baseVertex;
    uint material;      // its material row, ~0 for a batch that is not drawn
    uint place;         // its kind (bits 0-2) and its place among the group's of that kind
    uint caster;        // its place among the group's casters, ~0 for one that casts nothing
    uint texture;       // what it casts with: its texture's handle,
    uint casterFlags;   // and bit 0 sample it, bit 1 alpha-test it
};
const uint kNone = 0xFFFFFFFFu;
const uint kColourKinds = 6u;
const uint kBlendedKinds = 4u;   // kinds 4 and 5 are the blended layer
const uint kAlphaKeyRef = 224u;  // where a building's alpha-keyed shadow cuts, in 255ths

layout(std430, set = 0, binding = 0) readonly buffer Instances { Instance instances[]; };
layout(std430, set = 0, binding = 1) readonly buffer Groups { Group groups[]; };
layout(std430, set = 0, binding = 2) readonly buffer Ranges { Range ranges[]; };

// WmoCullFrameGPU.
layout(std140, set = 0, binding = 3) uniform Frame {
    vec4 viewPlanes[6];
    vec4 mirrorPlanes[6];
    vec4 camera;        // xyz the eye, w the horizontal view distance
    vec4 limits;        // x the group distance, negative for none; y the mirror's height; z 1 with a mirror
    uvec4 counts;       // x group rows, y lights, z places a colour list, w places a shadow map
    uvec4 shape;        // x geometry pages, y 1 when the portal walk ran
} frame;

// Each bucket's first place: the colour buckets by page and kind, then the
// casters by page.
layout(std430, set = 0, binding = 4) readonly buffer BucketStarts { uint bucketStarts[]; };
// A bit a group row: the portal walk left it out.
layout(std430, set = 0, binding = 5) readonly buffer PortalCulled { uint portalCulled[]; };

#include "draw_command.glsli"
// The view's list, the reflection's, then each shadow map's.
layout(std430, set = 0, binding = 6) writeonly buffer Commands { DrawCommand commands[]; };
// By colour place: the draw's material row and group row (wmo.vert).
layout(std430, set = 0, binding = 7) writeonly buffer ColourRecords { uvec2 colourRecords[]; };
// By caster place: texture, flags, alpha ref, group row (wmo_shadow.vert).
layout(std430, set = 0, binding = 8) writeonly buffer CasterRecords { uvec4 casterRecords[]; };
// By group row: the lights that reach it this frame.
layout(std430, set = 0, binding = 9) writeonly buffer GroupLights { uvec4 groupLights[]; };

// What was drawn, for the frame's statistics (WmoCullStatsGPU).
layout(std430, set = 0, binding = 10) buffer Stats {
    uint viewDraws;
    uint blendedDraws;
    uint reflectionDraws;
    uint casterDraws;
    uint drawnGroups;
    uint frustumCulled;
    uint distanceCulled;
    uint portalCulled;
} stats;

layout(std430, set = 0, binding = 11) readonly buffer LocalLightBuffer { vec4 localLightData[]; };

#include "local_lights_pick.glsli"

// x 0 the colour lists, 1 a shadow map; y the map's number.
// depth: the map's centre and how far from it a caster reaches it.
layout(push_constant) uniform Pass {
    vec4 depth;
    uvec4 mode;
} pass;

// Frustum::intersectsAABB: outside when the box's corner furthest along a
// plane's normal is behind it.
bool boxInside(vec4 planes[6], vec3 boxMin, vec3 boxMax) {
    for (int i = 0; i < 6; ++i) {
        vec3 positive = vec3(planes[i].x >= 0.0 ? boxMax.x : boxMin.x, planes[i].y >= 0.0 ? boxMax.y : boxMin.y,
                             planes[i].z >= 0.0 ? boxMax.z : boxMin.z);
        precise float distance = dot(planes[i].xyz, positive) + planes[i].w;
        if (distance < 0.0) return false;
    }
    return true;
}

uint colourSlot(Group g, uint kind) {
    return kind < 4u ? g.slots0[kind] : g.slots1[kind - 4u];
}

void colour(uint index, Group g, Instance instance) {
    const vec3 boxMin = g.worldMin.xyz;
    const vec3 boxMax = g.worldMax.xyz;
    const vec3 camPos = frame.camera.xyz;
    if (frame.shape.y != 0u && (portalCulled[index >> 5] & (1u << (index & 31u))) != 0u) {
        atomicAdd(stats.portalCulled, 1u);
        return;
    }
    const vec2 nearest = clamp(camPos.xy, boxMin.xy, boxMax.xy);
    const vec2 delta = nearest - camPos.xy;
    if (dot(delta, delta) > frame.camera.w * frame.camera.w) {
        atomicAdd(stats.distanceCulled, 1u);
        return;
    }
    if (frame.limits.x >= 0.0) {
        const vec3 closest = clamp(camPos, boxMin, boxMax);
        if (dot(closest - camPos, closest - camPos) > frame.limits.x * frame.limits.x) {
            atomicAdd(stats.distanceCulled, 1u);
            return;
        }
    }
    // The view keeps what is in the camera's volume; the reflection what
    // stands where the mirror shows it, banners swung as far as they go.
    const bool inView = boxInside(frame.viewPlanes, boxMin, boxMax);
    const vec3 sway = vec3(g.worldMin.w);
    const bool inMirror = frame.limits.z != 0.0 && boxMax.z + sway.z >= frame.limits.y &&
                          boxInside(frame.mirrorPlanes, boxMin - sway, boxMax + sway);
    if (!inView) atomicAdd(stats.frustumCulled, 1u);
    if (!inView && !inMirror) return;
    if ((g.refs.w & kGroupAntiportal) != 0u) return;
    // A distance-only shell stands in for the building from afar.
    if ((g.refs.w & kGroupShell) != 0u) {
        const vec3 centre = (instance.model * vec4(g.localCentre.xyz, 1.0)).xyz;
        if (dot(camPos - centre, camPos - centre) < 196.0 * 196.0) return;
    }
    if (inView) atomicAdd(stats.drawnGroups, 1u);

    groupLights[index] = pickLocalLights((boxMin + boxMax) * 0.5, length(boxMax - boxMin) * 0.5, frame.counts.y);
    const uint page = g.slots1.w;
    for (uint r = g.refs.y; r < g.refs.y + g.refs.z; ++r) {
        const Range range = ranges[r];
        if (range.material == kNone) continue;
        const uint kind = range.place & 7u;
        const uint slot = bucketStarts[page * kColourKinds + kind] + colourSlot(g, kind) + (range.place >> 3);
        colourRecords[slot] = uvec2(range.material, index);
        const DrawCommand c = indexedDraw(range.indexCount, range.firstIndex, range.baseVertex);
        if (inView) {
            commands[slot] = c;
            if (kind >= kBlendedKinds) atomicAdd(stats.blendedDraws, 1u);
            else atomicAdd(stats.viewDraws, 1u);
        }
        if (inMirror) {
            commands[frame.counts.z + slot] = c;
            atomicAdd(stats.reflectionDraws, 1u);
        }
    }
}

void depth(uint index, Group g) {
    if ((g.refs.w & (kGroupShell | kGroupAntiportal)) != 0u) return;
    const vec3 centre = pass.depth.xyz;
    const vec3 nearest = clamp(centre, g.worldMin.xyz, g.worldMax.xyz);
    if (dot(nearest - centre, nearest - centre) > pass.depth.w * pass.depth.w) return;
    const uint page = g.slots1.w;
    const uint map = 2u * frame.counts.z + pass.mode.y * frame.counts.w;
    const uint first = bucketStarts[frame.shape.x * kColourKinds + page] + g.slots1.z;
    for (uint r = g.refs.y; r < g.refs.y + g.refs.z; ++r) {
        const Range range = ranges[r];
        if (range.caster == kNone) continue;
        const uint slot = first + range.caster;
        casterRecords[slot] = uvec4(range.texture, range.casterFlags,
                                    (range.casterFlags & 2u) != 0u ? kAlphaKeyRef : 0u, index);
        commands[map + slot] = indexedDraw(range.indexCount, range.firstIndex, range.baseVertex);
        atomicAdd(stats.casterDraws, 1u);
    }
}

void main() {
    const uint index = gl_GlobalInvocationID.x;
    if (index >= frame.counts.x) return;
    const Group g = groups[index];
    if ((g.refs.w & kGroupValid) == 0u) return;
    const Instance instance = instances[g.refs.x];
    // Out of the world: neither drawn nor casting (setInstanceHidden).
    if (instance.state.x == 0u) return;
    if (pass.mode.x == 0u) colour(index, g, instance);
    else depth(index, g);
}
