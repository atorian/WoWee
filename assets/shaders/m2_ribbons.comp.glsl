#version 450

// The M2 ribbons, laid and drawn on the GPU - the only place they are
// (M2ParticlePass). A workgroup runs one ribbon emitter of one instance for
// the frame, as the original client's CRibbonEmitter does (SetPos, Update
// and InterpEdge; see Deamon87/WebWowViewerCpp, CRibbonEmitter.cpp, which
// is that client's code):
//
// - An edge is laid across the emitter's own Y axis and keeps that
//   orientation: trails from a turning bone twist, the phoenix's wing
//   trails are flat bands winding round.
// - New edges follow the curve from the last position to this one, bent by
//   the emitter's Z axis at both ends, so a fast bone leaves a smooth arc.
// - Gravity moves an edge by g * age^2 (z += (2g*age + g*dt)*dt), up for a
//   negative g.
// - The tracks are sampled at the animation's time, defaulting to 1; an
//   edge's alpha does not fade with age, the texture does that along
//   u = age / lifetime.
//
// Seen, its edges are laid out oldest first and the one at the emitter last,
// below then above each, as a triangle strip (m2_ribbon.vert), in a range of
// the frame's ribbon vertices, with a draw command.
//
// The range (particle_pool.hpp) holds two header records - the tag, whether
// it has a position yet, the fraction of an edge carried over, how many
// edges and where the oldest is; where the emitter was, its Z and Y axes
// there; and the edge at the emitter - and then a ring of edges: below xyz
// and age, above xyz and alpha, colour.

#include "m2_emitters.glsli"

// Nine words a vertex: position, colour, alpha, u, v.
layout(set = 0, binding = 7) writeonly buffer RibbonVertices {
    float ribbonVertices[];
};

// The emitter blob (skeleton_pool.hpp).
const uint kHeaderRibbons = 5u;
const uint kRibbonWords = 16u;
const uint kRibbonTracks = 8u;

uint ribbonRecord;
uint rb(uint k) { return word(ribbonRecord + k); }
float rbf(uint k) { return wordf(ribbonRecord + k); }

// Edge i of the ring is record 2 + i.
struct Edge {
    vec3 below;
    float age;
    vec3 above;
    float alpha;
    vec3 color;
};
Edge edgeAt(uint record) {
    const vec4 a = particlePages[poolPage].records[(poolBase + record) * 4u + 0u];
    const vec4 b = particlePages[poolPage].records[(poolBase + record) * 4u + 1u];
    const vec4 c = particlePages[poolPage].records[(poolBase + record) * 4u + 2u];
    return Edge(a.xyz, a.w, b.xyz, b.w, c.rgb);
}
void setEdge(uint record, Edge e) {
    setRecord(record, 0u, vec4(e.below, e.age));
    setRecord(record, 1u, vec4(e.above, e.alpha));
    setRecord(record, 2u, vec4(e.color, 0.0));
}

shared uint sCount, sOldest, sFirstVertex, sDrawn;
shared bool sHasHead;
shared float sStep, sLifetime;

void writeVertex(uint at, vec3 position, Edge e, float u, float v) {
    const uint q = at * 9u;
    ribbonVertices[q + 0u] = position.x;
    ribbonVertices[q + 1u] = position.y;
    ribbonVertices[q + 2u] = position.z;
    ribbonVertices[q + 3u] = e.color.r;
    ribbonVertices[q + 4u] = e.color.g;
    ribbonVertices[q + 5u] = e.color.b;
    ribbonVertices[q + 6u] = e.alpha;
    ribbonVertices[q + 7u] = u;
    ribbonVertices[q + 8u] = v;
}

void main() {
    const uint lid = gl_LocalInvocationIndex;
    const uint rowIndex = push.firstRow + gl_WorkGroupID.x;
    const Row row = rows[rowIndex];
    pageIndex = row.blobPage;
    blobBase = row.blobFirst;
    ribbonRecord = word(kHeaderRibbons) + row.emitter * kRibbonWords;
    poolPage = row.poolPage;
    poolBase = row.poolFirst;
    const uint capacity = row.capacity;

    if (lid == 0u) {
        vec4 h0 = record(0u, 0u);
        vec4 lastPos = record(0u, 1u);
        vec4 lastDir = record(0u, 2u);
        vec4 lastVertical = record(0u, 3u);
        // A range under another tag is fresh: no position, no edges.
        if (floatBitsToUint(h0.x) != row.tag) {
            h0 = vec4(uintBitsToFloat(row.tag), 0.0, 0.0, 0.0);
            lastPos = vec4(0.0);
            lastDir = vec4(0.0);
            lastVertical = vec4(0.0);
        }
        bool posSet = h0.y != 0.0;
        float startTime = h0.z;
        uint count = floatBitsToUint(h0.w);
        uint oldest = floatBitsToUint(lastPos.w);
        bool hasHead = lastDir.w != 0.0;
        const float edgesPerSecond = rbf(4u);
        const float lifetime = rbf(5u);
        const float gravity = rbf(6u);
        float step = 0.0;

        // An emitter that does not run this frame keeps its edges as they are.
        const mat4 frame = emitterToWorld(row, rb(0u)) *
                           mat4(vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0, 1.0, 0.0, 0.0), vec4(0.0, 0.0, 1.0, 0.0),
                                vec4(rbf(1u), rbf(2u), rbf(3u), 1.0));
        const vec3 pos = frame[3].xyz;
        if (row.dt > 0.0 && !any(isnan(pos)) && !any(isinf(pos))) {
            const vec3 dir = frame[2].xyz;
            const vec3 vertical = frame[1].xyz;
            const uint tracks = ribbonRecord + kRibbonTracks;
            const vec3 color = sampleVec3(word(tracks + 0u), row.sequence, row.time, row.globalTime, vec3(1.0));
            const float alpha = sampleFloat(word(tracks + 1u), row.sequence, row.time, row.globalTime, 1.0);
            const float above = sampleFloat(word(tracks + 2u), row.sequence, row.time, row.globalTime, 1.0);
            const float below = sampleFloat(word(tracks + 3u), row.sequence, row.time, row.globalTime, 1.0);
            const bool visible =
                sampleFloat(word(tracks + 4u), row.sequence, row.time, row.globalTime, 1.0) > 0.5;

            const bool laying = (row.flags & kEmit) != 0u && visible;
            // The first update lays one edge whatever the step, as the
            // client's does, so a trail starts at once.
            const bool first = laying && !posSet;
            if (first) {
                lastPos.xyz = pos;
                lastDir.xyz = dir;
                lastVertical.xyz = vertical;
                startTime = 0.0;
                posSet = true;
            }
            step = clamp(first ? 1.0 / edgesPerSecond + 0.0001 : row.dt, 0.0, lifetime);

            // Those that would outlive their life go first.
            while (count > 0u && edgeAt(2u + oldest).age + step > lifetime) {
                oldest = (oldest + 1u) % capacity;
                --count;
            }

            if (laying) {
                const float scale = length(lastPos.xyz - pos);
                const vec3 below0 = lastPos.xyz - below * lastVertical.xyz;
                const vec3 below1 = pos - below * vertical;
                const vec3 above0 = lastPos.xyz + above * lastVertical.xyz;
                const vec3 above1 = pos + above * vertical;
                const vec3 lastDirScaled = scale * lastDir.xyz;
                const vec3 dirScaled = scale * dir;

                const float end = step * edgesPerSecond + startTime;
                if (end >= 1.0) {
                    // Born across this step: the earliest is the oldest, and
                    // the ageing below brings each to the time since its birth.
                    const float ooDenom = 1.0 / (end - startTime);
                    const int births = int(max(floor(end - 1.0), 0.0)) + 1;
                    for (int i = 1; i <= births; ++i) {
                        const float t = (float(i) - startTime) * ooDenom;
                        Edge e;
                        e.below = (below1 - (1.0 - t) * dirScaled) * t + (t * lastDirScaled + below0) * (1.0 - t);
                        e.above = (above1 - (1.0 - t) * dirScaled) * t + (t * lastDirScaled + above0) * (1.0 - t);
                        e.color = color;
                        e.alpha = alpha;
                        e.age = -t * step;
                        setEdge(2u + (oldest + count) % capacity, e);
                        ++count;
                        if (count >= capacity) {
                            oldest = (oldest + 1u) % capacity;
                            --count;
                        }
                    }
                }
                startTime = end - floor(end);
                Edge head;
                head.below = below1;
                head.above = above1;
                head.color = color;
                head.alpha = alpha;
                head.age = 0.0;
                setEdge(1u, head);
                hasHead = true;
                lastPos.xyz = pos;
                lastDir.xyz = dir;
                lastVertical.xyz = vertical;
            } else {
                hasHead = false;
            }
        }
        setRecord(0u, 0u, vec4(uintBitsToFloat(row.tag), posSet ? 1.0 : 0.0, startTime, uintBitsToFloat(count)));
        setRecord(0u, 1u, vec4(lastPos.xyz, uintBitsToFloat(oldest)));
        setRecord(0u, 2u, vec4(lastDir.xyz, hasHead ? 1.0 : 0.0));
        setRecord(0u, 3u, lastVertical);
        sCount = count;
        sOldest = oldest;
        sHasHead = hasHead;
        sStep = step;
        sLifetime = lifetime;
        read[rowIndex] = uvec2(count, 0u);
    }
    memoryBarrierBuffer();
    barrier();

    // Every edge ages, the newborn too, and rises or falls with gravity.
    const uint count = sCount;
    const uint oldest = sOldest;
    const float step = sStep;
    if (step > 0.0) {
        const float gravity = rbf(6u);
        for (uint k = lid; k < count; k += 64u) {
            const uint at = 2u + (oldest + k) % capacity;
            Edge e = edgeAt(at);
            const float rise = (2.0 * gravity * e.age + gravity * step) * step;
            e.below.z += rise;
            e.above.z += rise;
            e.age += step;
            setEdge(at, e);
        }
    }
    memoryBarrierBuffer();
    barrier();

    // Seen: a strip, oldest edge first and the head last, below then above.
    if ((row.flags & kInView) == 0u) return;
    const uint edges = count + (sHasHead ? 1u : 0u);
    if (lid == 0u) {
        const uint wanted = edges >= 2u ? edges * 2u : 0u;
        uint first = wanted > 0u ? atomicAdd(ribbonVerticesTaken, wanted) : 0u;
        uint taken = wanted;
        if (first + wanted > ribbonVertexRoom) {
            taken = first < ribbonVertexRoom ? ribbonVertexRoom - first : 0u;
            taken -= taken % 2u;
            atomicAdd(ribbonVerticesShort, wanted - taken);
        }
        // A strip of fewer than two edges is no strip.
        if (taken < 4u) taken = 0u;
        draws[rowIndex] = uvec4(taken, 1u, first, 0u);
        sFirstVertex = first;
        sDrawn = taken / 2u;
    }
    barrier();
    const uint drawn = sDrawn;
    const uint firstVertex = sFirstVertex;
    const float lifetime = sLifetime;
    for (uint k = lid; k < drawn; k += 64u) {
        const Edge e = k < count ? edgeAt(2u + (oldest + k) % capacity) : edgeAt(1u);
        const float u = max(e.age, 0.0) / lifetime;
        writeVertex(firstVertex + k * 2u, e.below, e, u, 0.0);
        writeVertex(firstVertex + k * 2u + 1u, e.above, e, u, 1.0);
    }
}
