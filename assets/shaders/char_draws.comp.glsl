#version 450

// The characters' draws made on the GPU (CharacterRenderer,
// character_draws.hpp), one thread per entry: a batch of an instance's draw
// list, which the main thread builds when the instance's dress, model or
// textures change.
//
// Every entry has places of its own, kept for as long as its list is: in
// the solid part, the place its batch is drawn at in a solid instance and,
// where a partly faded batch is drawn on another pipeline, a place for that;
// in the faded part, the place of its colour and, for a batch whose solid
// parts a faded body writes into depth first, the place of that; and a place
// among its page's shadow casters. A frame writes a draw at the place its
// instance's state picks, or leaves them empty - the commands are zeroed
// before - so a bucket keeps one order from frame to frame and is drawn
// whole by one indirect call. The buckets go by phase - how often the state
// changed before the batch in its instance's drawing order - so an
// instance's batches keep the order the CPU drew them in.
//
// The colour pass is CharacterRenderer's per-frame rules from before: an
// instance beyond the render radius or outside the view is not drawn
// (unless it is placed by another's matrix or is a scene); a batch whose
// animated alpha - its colour track times its texture weight - is at most
// 0.01 is not drawn; a faded instance (opacity under 0.999) is drawn in the
// faded part; in a solid one, a batch whose opacity is under 0.999 takes its
// partly-faded place. Its texture animations are sampled at the instance's
// clock, and its instance's lights picked (local_lights_pick.glsli) by its
// first entry. A shadow map's cull takes every caster whose instance stands
// within the map's reach of its centre.

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) readonly buffer Page {
    uint words[];
} pages[64];

#include "m2_track.glsli"
#include "m2_uv_transform.glsli"

// CharEntryGPU: a batch of an instance's draw list.
struct Entry {
    uint indexCount;
    uint firstIndex;
    int baseVertex;
    uint slot;          // its instance's slot
    uint material;      // its material row
    uint colorAlpha;    // its colour track, in the model's animation blob; ~0 for none
    uint weight;        // its texture weight track; ~0 for none
    uint flags;         // bit 0 valid, 1 its instance's first, 2 sample its caster's texture, 3 alpha-test it
    uvec2 uvTransforms; // each stage's texture animation; ~0 for none
    uint casterTexture;
    uint pad;
    // (bucket, offset) of each place; bucket ~0 for none.
    uvec2 solid;
    uvec2 partial;
    uvec2 fadedDepth;
    uvec2 faded;
    uvec2 caster;
    uvec2 pad2;
};
const uint kEntryValid = 1u;
const uint kEntryFirst = 2u;
const uint kCasterSample = 4u;
const uint kCasterAlphaTest = 8u;

// CharInstanceGPU (character_material.glsli).
struct CharInstance {
    uvec4 pose;             // w: bit 0 posed and placed, 1 culled by distance and view, 2 seen (opacity above 0)
    vec4 interiorAmbient;
    vec4 interiorDirect;
    vec4 stateTint;         // a its opacity
    vec4 sphere;            // xyz where it stands, w its cull radius
    vec4 clock;             // x animation ms, y global ms, z sequence (int bits)
    uvec4 anim;             // x its animation blob's page, y its first word; ~0 for none
    uvec4 pad;
};
const uint kInstancePlaced = 1u;
const uint kInstanceCulled = 2u;
const uint kInstanceSeen = 4u;

layout(std430, set = 1, binding = 0) readonly buffer Entries { Entry entries[]; };
layout(std430, set = 1, binding = 1) readonly buffer Instances { CharInstance instances[]; };

// CharCullFrameGPU.
layout(std140, set = 1, binding = 2) uniform Frame {
    vec4 viewPlanes[6];
    vec4 camera;        // xyz the eye, w the render radius squared
    uvec4 counts;       // x entries, y lights, z colour places (solid then faded), w caster places
} frame;

// Each bucket's first place: the colour buckets, then the casters'.
layout(std430, set = 1, binding = 3) readonly buffer BucketStarts { uint bucketStarts[]; };

#include "draw_command.glsli"
// The colour lists, then each shadow map's.
layout(std430, set = 1, binding = 4) writeonly buffer Commands { DrawCommand commands[]; };

// CharDrawGPU, by colour place.
struct CharDraw {
    uvec4 refs;
    vec4 uvRow0;
    vec4 uvRow1;
    vec4 uv2Row0;
    vec4 uv2Row1;
    vec4 opacity;
};
layout(std430, set = 1, binding = 5) writeonly buffer Draws { CharDraw draws[]; };
// By caster place: texture, flags, unused, the instance's slot.
layout(std430, set = 1, binding = 6) writeonly buffer CasterRecords { uvec4 casterRecords[]; };
// By instance slot: the lights that reach it this frame.
layout(std430, set = 1, binding = 7) writeonly buffer InstanceLights { uvec4 instanceLights[]; };

// What was drawn, for the frame's statistics (CharCullStatsGPU).
layout(std430, set = 1, binding = 8) buffer Stats {
    uint solidDraws;
    uint fadedDraws;
    uint casterDraws;
    uint drawnInstances;
} stats;

layout(std430, set = 1, binding = 9) readonly buffer LocalLightBuffer { vec4 localLightData[]; };

#include "local_lights_pick.glsli"

// x 0 the colour lists, 1 a shadow map; y the map's number. depth: the
// map's centre and how far from it an instance casts into it.
layout(push_constant) uniform Pass {
    vec4 depth;
    uvec4 mode;
} pass;

// The character animation blob (packCharacterAnimations): words 5 and 6
// the colour tracks' count and table, 7 and 8 the texture weights'.
const uint kBlobColourCount = 5u;
const uint kBlobColours = 6u;
const uint kBlobWeightCount = 7u;
const uint kBlobWeights = 8u;

bool sphereInside(vec3 centre, float radius) {
    for (int i = 0; i < 6; ++i) {
        precise float distance = dot(frame.viewPlanes[i].xyz, centre) + frame.viewPlanes[i].w;
        if (distance < -radius) return false;
    }
    return true;
}

uint placeOf(uvec2 place) {
    return place.x == kNone ? kNone : bucketStarts[place.x] + place.y;
}

void main() {
    const uint index = gl_GlobalInvocationID.x;
    if (index >= frame.counts.x) return;
    const Entry e = entries[index];
    if ((e.flags & kEntryValid) == 0u) return;
    const CharInstance instance = instances[e.slot];
    if ((instance.pose.w & kInstancePlaced) == 0u) return;

    if (pass.mode.x != 0u) {
        const uint place = placeOf(e.caster);
        if (place == kNone) return;
        const vec3 toCentre = instance.sphere.xyz - pass.depth.xyz;
        if (dot(toCentre, toCentre) > pass.depth.w * pass.depth.w) return;
        casterRecords[place] = uvec4(e.casterTexture,
                                     ((e.flags & kCasterSample) != 0u ? 1u : 0u) |
                                     ((e.flags & kCasterAlphaTest) != 0u ? 2u : 0u), 0u, e.slot);
        commands[frame.counts.z + pass.mode.y * frame.counts.w + place] = indexedDraw(e.indexCount, e.firstIndex, e.baseVertex);
        atomicAdd(stats.casterDraws, 1u);
        return;
    }

    if ((instance.pose.w & kInstanceSeen) == 0u) return;
    if ((instance.pose.w & kInstanceCulled) != 0u) {
        const vec3 toInstance = instance.sphere.xyz - frame.camera.xyz;
        if (dot(toInstance, toInstance) > frame.camera.w) return;
        if (!sphereInside(instance.sphere.xyz, instance.sphere.w)) return;
    }
    if ((e.flags & kEntryFirst) != 0u) {
        // The lights nearest the unit's origin, as the client picks them for
        // any model.
        instanceLights[e.slot] = pickLocalLights(instance.sphere.xyz, 0.0, frame.counts.y);
        atomicAdd(stats.drawnInstances, 1u);
    }

    // Its animated alpha and texture animations, at its clock.
    const int sequence = floatBitsToInt(instance.clock.z);
    float alpha = 1.0;
    CharDraw draw;
    draw.uvRow0 = vec4(1.0, 0.0, 0.0, 0.0);
    draw.uvRow1 = vec4(0.0, 1.0, 0.0, 0.0);
    draw.uv2Row0 = draw.uvRow0;
    draw.uv2Row1 = draw.uvRow1;
    if (instance.anim.x != kNone) {
        pageIndex = instance.anim.x;
        blobBase = instance.anim.y;
        float colour = 1.0;
        float weight = 1.0;
        if (e.colorAlpha != kNone && e.colorAlpha < word(kBlobColourCount))
            colour = sampleFloat(word(word(kBlobColours) + e.colorAlpha), sequence, instance.clock.x,
                                 instance.clock.y, 1.0);
        if (e.weight != kNone && e.weight < word(kBlobWeightCount))
            weight = sampleFloat(word(word(kBlobWeights) + e.weight), sequence, instance.clock.x,
                                 instance.clock.y, 1.0);
        alpha = clamp(colour * weight, 0.0, 1.0);
        uvTransform(e.uvTransforms.x, sequence, instance.clock.x, instance.clock.y, draw.uvRow0, draw.uvRow1);
        uvTransform(e.uvTransforms.y, sequence, instance.clock.x, instance.clock.y, draw.uv2Row0, draw.uv2Row1);
    }
    if (alpha <= 0.01) return;
    const float opacity = instance.stateTint.a * alpha;
    draw.refs = uvec4(e.material, e.slot, 0u, 0u);
    draw.opacity = vec4(opacity, 0.0, 0.0, 0.0);
    const DrawCommand c = indexedDraw(e.indexCount, e.firstIndex, e.baseVertex);

    if (instance.stateTint.a < 0.999) {
        // A faded body: its colour, and its solid parts into depth first.
        const uint place = placeOf(e.faded);
        if (place != kNone) {
            draws[place] = draw;
            commands[place] = c;
            atomicAdd(stats.fadedDraws, 1u);
        }
        const uint depthPlace = placeOf(e.fadedDepth);
        if (depthPlace != kNone) {
            draws[depthPlace] = draw;
            commands[depthPlace] = c;
            atomicAdd(stats.fadedDraws, 1u);
        }
        return;
    }
    const uint partial = placeOf(e.partial);
    const uint place = opacity < 0.999 && partial != kNone ? partial : placeOf(e.solid);
    if (place == kNone) return;
    draws[place] = draw;
    commands[place] = c;
    atomicAdd(stats.solidDraws, 1u);
}
