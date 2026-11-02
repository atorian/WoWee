#version 450

// The M2 particles, run and laid out on the GPU - the only place they are
// (ParticlePool, M2ParticlePass). A workgroup runs one emitter of one
// instance for the frame, as the 3.3.5a client does (CParticleEmitter2;
// m2_particle_emission.hpp and docs/wow-client-render-spec.md, sections 4
// and 6.1, say the rules):
//
// - Its tracks are sampled at the instance's time (m2_track.glsli), and it
//   stands where its bone was posed this frame (skeleton.comp), on its
//   instance's model or on a creature's (CharacterRenderer).
// - A long frame is played in steps of at most 0.1 s, no more than its
//   longest life's worth. Each step it emits - its rate, give or take its
//   variation, thinned by the density setting and its distance, into the
//   accumulated fraction, rounded; no more than its pool holds - and then
//   moves every particle and lets those past their life go.
// - Seen, its particles are laid out as quads for m2_particle.vert, at most
//   its draw cap, in a range of the frame's quads taken as it goes, and a
//   draw command for them.
//
// Particles are born into the free records in the order of the records and
// laid out in that order, and every draw is made of the particle's own
// number, so a scene runs the same every time.

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) readonly buffer Page {
    uint words[];
} pages[64];

#include "m2_track.glsli"

// Four vec4 a record (particle_pool.hpp): the header, then the particles.
layout(set = 0, binding = 1) buffer ParticlePage {
    vec4 records[];
} particlePages[64];

struct Row {
    uint blobPage;       // the model's emitter blob
    uint blobFirst;
    uint emitter;        // which of them
    uint flags;          // kEmit, kInView, kFirefly, kLit, kM2Skeleton, kCharacterSkeleton
    uint poolPage;       // its range of records: the header's
    uint poolFirst;
    uint capacity;       // particles it has room for
    uint tag;            // the range's, as the pool gave it
    int sequence;        // the instance's sequence and times, ms
    float time;
    float globalTime;
    float dt;            // seconds since the frame before
    uint request;        // its skeleton in the frame's posed bones
    uint boneBase;
    uint boneCount;
    uint seed;           // its own, fixed for its life
    uint frameSerial;    // this frame's, for the rate's draw
    uint pad0, pad1, pad2;
    vec4 cullCentre;     // xyz: its instance's cull centre; w: its fade
    mat4 placement;      // its model matrix, where it has no skeleton posed
    vec4 lightAmbient;   // its instance's interior light (InteriorLightGPU)
    vec4 lightDirect;
    uvec4 localLights;   // its instance's point lights (local_lights.glsli)
};
layout(set = 0, binding = 2) readonly buffer Rows {
    Row rows[];
};
// M2ParticleQuad, 31 words each.
layout(set = 0, binding = 3) writeonly buffer Quads {
    float quads[];
};
// A VkDrawIndirectCommand a row.
layout(set = 0, binding = 4) writeonly buffer Draws {
    uvec4 draws[];
};
// x: quads taken, y: quads there were room for, z: quads wanted beyond it.
layout(set = 0, binding = 5) coherent buffer Counters {
    uint quadsTaken;
    uint quadRoom;
    uint quadsShort;
};
// A row's live particles and how far the farthest is from its instance.
layout(set = 0, binding = 6) writeonly buffer Read {
    uvec2 read[];
};

// The frame's posed bones: the doodads' (binding 0 bones, 1 models) and the
// creatures'.
layout(set = 1, binding = 0) readonly buffer M2Bones { mat4 m2Bones[]; };
layout(set = 1, binding = 1) readonly buffer M2Models { mat4 m2Models[]; };
layout(set = 2, binding = 0) readonly buffer CharacterBones { mat4 characterBones[]; };
layout(set = 2, binding = 1) readonly buffer CharacterModels { mat4 characterModels[]; };

layout(push_constant) uniform Push {
    vec4 eye;            // xyz the camera; w the particle density setting
    uint firstRow;
    uint charactersPosed;
    uint pad0, pad1;
} push;

const uint kEmit = 1u;
const uint kInView = 2u;
const uint kFirefly = 4u;
const uint kLit = 8u;
const uint kM2Skeleton = 16u;
const uint kCharacterSkeleton = 32u;

// The emitter blob (skeleton_pool.hpp).
const uint kHeaderEmitters = 2u;
const uint kEmitterWords = 48u;
const uint kEmitterTracks = 28u;
const uint kEmitterFBlocks = 38u;

// Emitter flags (m2_loader.hpp).
const uint kFlagAlongVelocity = 0x4u;
const uint kFlagModelSpace = 0x10u;
const uint kFlagBoneScale = 0x20u;
const uint kFlagSphereUp = 0x100u;
const uint kFlagRandomSpin = 0x200u;
const uint kFlagClampTail = 0x400u;
const uint kFlagPlane = 0x1000u;
const uint kFlagRandomCell = 0x10000u;
const uint kFlagIndependentScale = 0x80000u;

const float kMaxStep = 0.1;
const float kAlongVelocityMinSpeedSq = 2.4e-7;

// M2ParticleShape.
const uint kShapeBillboard = 0u;
const uint kShapeTail = 1u;
const uint kShapeAlongVelocity = 2u;
const uint kShapePlane = 3u;

uint emitterRecord;
uint poolPage;
uint poolBase;

vec4 record(uint index, uint part) { return particlePages[poolPage].records[(poolBase + index) * 4u + part]; }
void setRecord(uint index, uint part, vec4 value) { particlePages[poolPage].records[(poolBase + index) * 4u + part] = value; }
// Particle i is record 1 + i.
bool alive(uint i) { return record(1u + i, 1u).w > 0.0; }

uint em(uint k) { return word(emitterRecord + k); }
float emf(uint k) { return wordf(emitterRecord + k); }

// ---- Random numbers: a hash of the emitter's seed and a count ----

uint hash(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
uint rngState;
uint randUint() { rngState = hash(rngState); return rngState; }
float rand01() { return float(randUint() >> 8u) / 16777216.0; }
float randN() { return rand01() * 2.0 - 1.0; }

// ---- The client's rules (m2_particle_emission.hpp) ----

float emissionDistanceLod(float distance) { return clamp(1.0 - (distance - 50.0) * 0.02, 0.25, 1.0); }

uint emitterPool(float rate, float rateVary, float life, float lifeVary) {
    const float n = (max(0.0, rate) + max(0.0, rateVary)) * (max(0.0, life) + max(0.0, lifeVary)) * 1.15;
    return n > 0.0 ? uint(n) : 0u;
}

vec3 emitterToBone(vec3 v) { return vec3(-v.y, v.x, v.z); }

vec3 coneDirection(float polar, float azimuth) {
    return vec3(sin(polar) * cos(azimuth), sin(polar) * sin(azimuth), cos(polar));
}

vec3 sphereSpawnOffset(float minRadius, float maxRadius, float r01, float elevation, float azimuth) {
    const float r = minRadius + r01 * (maxRadius - minRadius);
    return r * vec3(cos(elevation) * cos(azimuth), cos(elevation) * sin(azimuth), sin(elevation));
}

vec3 zSourceDirection(vec3 offset, float zSource) {
    const vec3 d = offset - vec3(0.0, 0.0, zSource);
    const float len = length(d);
    return len > 0.0 ? d / len : vec3(0.0);
}

float particleSpin(float baseSpin, float baseSpinVary, float spin, float spinVary, float randBase,
                   float randSpin, float age) {
    if (spin == 0.0 && spinVary == 0.0) return 0.0;
    return baseSpin + randBase * baseSpinVary + age * (spin + randSpin * spinVary);
}

// The twinkle table's slot k: the k + 1th draw of the client's table
// generator from its seed, reached by jumping the generator ahead.
float twinkleAt(uint slot) {
    uint accMul = 1u, accAdd = 0u, curMul = 1664525u, curAdd = 1013904223u;
    for (uint n = slot + 1u; n > 0u; n >>= 1u) {
        if ((n & 1u) != 0u) {
            accMul *= curMul;
            accAdd = accAdd * curMul + curAdd;
        }
        curAdd = (curMul + 1u) * curAdd;
        curMul *= curMul;
    }
    return float((accMul * 0x12345678u + accAdd) >> 8u) / 16777216.0;
}
uint twinkleSlot(float twinkleSpeed, float age, uint offset) {
    return (uint(int(twinkleSpeed * age)) + offset) & 127u;
}

// An FBlock at a fraction t of a particle's life (m2FBlockSpan): two keys
// ignore their times, three use only the middle one, more are searched.
vec4 fblock(uint block, float t, vec4 none) {
    if (block == kNone) return none;
    const uint keys = word(block);
    const uint timeCount = word(block + 1u);
    const uint times = block + 2u;
    const uint values = times + timeCount;
    uint from = 0u, to = 0u;
    float frac = 0.0;
    if (keys <= 1u) {
    } else if (keys == 2u) {
        to = 1u; frac = t;
    } else if (keys == 3u) {
        const float m = timeCount > 1u ? wordf(times + 1u) : 0.5;
        if (t < m) { to = 1u; frac = m > 0.0 ? t / m : 0.0; }
        else { from = 1u; to = 2u; frac = m < 1.0 ? (t - m) / (1.0 - m) : 0.0; }
    } else {
        const uint n = min(keys, timeCount);
        if (n < 2u || t <= wordf(times)) {
        } else if (t >= wordf(times + n - 1u)) {
            from = to = n - 1u;
        } else {
            to = 1u;
            while (to < n && wordf(times + to) <= t) ++to;
            from = to - 1u;
            const float span = wordf(times + to) - wordf(times + from);
            frac = span > 0.0 ? (t - wordf(times + from)) / span : 0.0;
        }
    }
    return mix(word4(values + from * 4u), word4(values + to * 4u), frac);
}
// An atlas cell track: rounded to the nearest, kept within the atlas.
float fblockCell(uint block, float t, uint cells) {
    if (cells == 0u) return 0.0;
    return clamp(round(fblock(block, t, vec4(0.0)).x), 0.0, float(cells - 1u));
}

// ---- One workgroup's shared state ----

shared mat4 sToWorld;
shared float sSpeed, sSpeedVary, sVertical, sHorizontal, sGravity, sLife, sAreaLength, sAreaWidth, sZSource;
shared float sRate, sAccumulated;
shared uint sPool, sSpawned, sSpawnNow, sLive, sReach, sVisible, sQuadFirst;
shared bool sEmits;
shared uint scan[64];

// The exclusive prefix sum of `value` over the workgroup, in invocation
// order, and the sum of all. Every invocation calls it.
uint prefixSum(uint value, out uint total) {
    const uint lid = gl_LocalInvocationIndex;
    scan[lid] = value;
    barrier();
    for (uint offset = 1u; offset < 64u; offset <<= 1u) {
        const uint add = lid >= offset ? scan[lid - offset] : 0u;
        barrier();
        scan[lid] += add;
        barrier();
    }
    total = scan[63];
    const uint inclusive = scan[lid];
    barrier();
    return inclusive - value;
}

// Born into record 1 + i, the emitter's `number`th particle.
void spawn(Row row, uint i, uint number, float step) {
    rngState = hash(row.seed ^ hash(number * 0x9e3779b9u + 0x632be5abu));
    const uint flags = em(0u);
    const vec2 scaleVary = vec2(emf(10u), emf(11u));
    const float life = max(sLife, 0.001);
    const float age = rand01() * step;
    const float sizeX = max(1e-4, 1.0 + randN() * scaleVary.x);
    const float sizeYDraw = randN();
    const float sizeY = (flags & kFlagIndependentScale) != 0u ? max(1e-4, 1.0 + sizeYDraw * scaleVary.y) : sizeX;
    const float spinRandBase = randN();
    const float spinRandRate = randN();
    const float spinSign = (flags & kFlagRandomSpin) != 0u && rand01() < 0.5 ? -1.0 : 1.0;
    const uint twinkleOffset = randUint() & 127u;

    // Where it starts and which way it goes, in the emitter's space: a plane
    // spreads them over its area into a cone about +Z; a sphere places them
    // between its radii and sends them out from its centre, or up with
    // flag 0x100; a spline (not done) sends them from its point; a zSource
    // sends either away from that point on the axis.
    const float speed = sSpeed * (1.0 + randN() * sSpeedVary);
    vec3 offset = vec3(0.0);
    vec3 dir = vec3(0.0, 0.0, 1.0);
    const uint type = em(1u);
    if (type == 2u) {
        const float r01 = rand01();
        const float elevation = randN() * sVertical;
        const float azimuth = randN() * sHorizontal;
        offset = sphereSpawnOffset(sAreaLength, sAreaWidth, r01, elevation, azimuth);
        if ((flags & kFlagSphereUp) == 0u) {
            const float len = length(offset);
            dir = len > 0.0 ? offset / len : vec3(0.0, 0.0, 1.0);
        }
    } else {
        if (type == 1u) {
            const float x = randN();
            const float y = randN();
            offset = vec3(x * sAreaLength * 0.5, y * sAreaWidth * 0.5, 0.0);
        }
        const float polar = randN() * sVertical;
        const float azimuth = randN() * sHorizontal;
        dir = coneDirection(polar, azimuth);
    }
    if (sZSource != 0.0) dir = zSourceDirection(offset, sZSource);
    const vec3 localPos = vec3(emf(4u), emf(5u), emf(6u)) + emitterToBone(offset);
    dir = emitterToBone(dir);

    // Into the world through the emitter's bone, or kept in its space to be
    // carried by it (flag 0x10).
    const bool modelSpace = (flags & kFlagModelSpace) != 0u;
    vec3 position = modelSpace ? localPos : (sToWorld * vec4(localPos, 1.0)).xyz;
    vec3 velocity = modelSpace ? dir * speed : mat3(sToWorld) * dir * speed;
    // Fireflies' bones move them in .anim files that are not loaded: one
    // standing still drifts of its own. Nothing else that stands still moves.
    if (abs(speed) < 0.01 && (row.flags & kFirefly) != 0u) {
        const mat3 rot = modelSpace ? mat3(1.0) : mat3(sToWorld);
        const float dx = randN(), dy = randN(), dz = randN();
        velocity = rot * vec3(dx * 0.6, dy * 0.6, dz * 0.3);
    }
    float tile = 0.0;
    const uint tiles = (em(3u) & 0xFFFFu) * (em(3u) >> 16u);
    if ((flags & kFlagRandomCell) != 0u && tiles > 1u) tile = float(randUint() % tiles);

    setRecord(1u + i, 0u, vec4(position, age));
    setRecord(1u + i, 1u, vec4(velocity, life));
    setRecord(1u + i, 2u, vec4(sizeX, sizeY, spinRandBase, spinRandRate));
    setRecord(1u + i, 3u, vec4(spinSign, uintBitsToFloat(twinkleOffset), tile, 0.0));
}

// One step of a particle's flight (0x979bb0): wind while it is young
// enough, gravity along -Z integrated exactly over the step, then drag.
void move(uint i, float step) {
    vec4 a = record(1u + i, 0u);
    vec4 b = record(1u + i, 1u);
    vec3 position = a.xyz;
    vec3 velocity = b.xyz;
    const float age = a.w;
    if (age < emf(23u)) velocity += vec3(emf(20u), emf(21u), emf(22u)) * step;
    position.xy += velocity.xy * step;
    position.z += velocity.z * step - 0.5 * sGravity * step * step;
    velocity.z -= sGravity * step;
    velocity -= velocity * min(1.0, emf(7u) * step);
    const float older = age + step;
    // Past its life it goes.
    setRecord(1u + i, 0u, vec4(position, older));
    setRecord(1u + i, 1u, vec4(velocity, older >= b.w ? 0.0 : b.w));
}

void writeQuad(uint at, vec3 position, vec4 color, vec2 halfSize, float cell, vec3 axisA, vec3 axisB, float spin,
               uint shape, Row row) {
    const uint q = at * 31u;
    quads[q + 0u] = position.x; quads[q + 1u] = position.y; quads[q + 2u] = position.z;
    quads[q + 3u] = color.r; quads[q + 4u] = color.g; quads[q + 5u] = color.b; quads[q + 6u] = color.a;
    quads[q + 7u] = halfSize.x; quads[q + 8u] = halfSize.y;
    quads[q + 9u] = cell;
    quads[q + 10u] = axisA.x; quads[q + 11u] = axisA.y; quads[q + 12u] = axisA.z;
    quads[q + 13u] = axisB.x; quads[q + 14u] = axisB.y; quads[q + 15u] = axisB.z;
    quads[q + 16u] = spin;
    quads[q + 17u] = uintBitsToFloat(shape);
    quads[q + 18u] = uintBitsToFloat((row.flags & kLit) != 0u ? 1u : 0u);
    for (uint k = 0u; k < 4u; ++k) {
        quads[q + 19u + k] = row.lightAmbient[k];
        quads[q + 23u + k] = row.lightDirect[k];
        quads[q + 27u + k] = uintBitsToFloat(row.localLights[k]);
    }
}

void main() {
    const uint lid = gl_LocalInvocationIndex;
    const uint rowIndex = push.firstRow + gl_WorkGroupID.x;
    const Row row = rows[rowIndex];
    pageIndex = row.blobPage;
    blobBase = row.blobFirst;
    emitterRecord = word(kHeaderEmitters) + row.emitter * kEmitterWords;
    poolPage = row.poolPage;
    poolBase = row.poolFirst;
    const uint capacity = row.capacity;
    const uint flags = em(0u);
    const uint chunks = (capacity + 63u) / 64u;

    // A range under another tag is fresh: nothing alive in it yet.
    const vec4 header = record(0u, 0u);
    const bool fresh = floatBitsToUint(header.x) != row.tag;
    if (fresh) {
        for (uint i = lid; i < capacity; i += 64u) setRecord(1u + i, 1u, vec4(0.0));
    }

    if (lid == 0u) {
        sAccumulated = fresh ? 0.0 : header.y;
        sPool = fresh ? 0u : floatBitsToUint(header.z);
        sSpawned = fresh ? 0u : floatBitsToUint(header.w);
        // Where it stands: its instance's model, then its bone.
        mat4 model = row.placement;
        mat4 bone = mat4(1.0);
        const uint b = em(2u);
        if ((row.flags & kM2Skeleton) != 0u) {
            model = m2Models[row.request];
            if (b < row.boneCount) bone = m2Bones[row.boneBase + b];
        } else if ((row.flags & kCharacterSkeleton) != 0u && push.charactersPosed != 0u) {
            model = characterModels[row.request];
            if (b < row.boneCount) bone = characterBones[row.boneBase + b];
        }
        sToWorld = model * bone;
        const uint tracks = emitterRecord + kEmitterTracks;
        sSpeed = sampleFloat(word(tracks + 0u), row.sequence, row.time, row.globalTime, 0.0);
        sSpeedVary = sampleFloat(word(tracks + 1u), row.sequence, row.time, row.globalTime, 0.0);
        sVertical = sampleFloat(word(tracks + 2u), row.sequence, row.time, row.globalTime, 0.0);
        sHorizontal = sampleFloat(word(tracks + 3u), row.sequence, row.time, row.globalTime, 0.0);
        sGravity = sampleFloat(word(tracks + 4u), row.sequence, row.time, row.globalTime, 0.0);
        sLife = sampleFloat(word(tracks + 5u), row.sequence, row.time, row.globalTime, 0.0);
        sRate = sampleFloat(word(tracks + 6u), row.sequence, row.time, row.globalTime, 0.0);
        sAreaLength = sampleFloat(word(tracks + 7u), row.sequence, row.time, row.globalTime, 0.0);
        sAreaWidth = sampleFloat(word(tracks + 8u), row.sequence, row.time, row.globalTime, 0.0);
        sZSource = sampleFloat(word(tracks + 9u), row.sequence, row.time, row.globalTime, 0.0);
        // The pool it has needed, never shrunk; within its room.
        sPool = min(max(sPool, emitterPool(sRate, emf(25u), sLife, emf(24u))), capacity);
        sEmits = (row.flags & kEmit) != 0u && (em(9u) & 4u) != 0u && sLife > 0.0;
    }
    barrier();

    // A long frame in steps of at most 0.1 s, no more than its longest
    // life's worth.
    const float longest = sLife + emf(24u);
    float remaining = longest > 0.0 ? min(row.dt, longest) : row.dt;
    const vec3 emitterWorld = (sToWorld * vec4(emf(4u), emf(5u), emf(6u), 1.0)).xyz;
    uint stepNumber = 0u;
    while (remaining > 0.0) {
        const float step = min(remaining, kMaxStep);
        remaining -= step;

        // How many are alive, against the pool.
        if (lid == 0u) sLive = 0u;
        barrier();
        uint mine = 0u;
        for (uint i = lid; i < capacity; i += 64u) mine += alive(i) ? 1u : 0u;
        atomicAdd(sLive, mine);
        barrier();

        // Emitted: the rate, give or take its variation (one draw a step),
        // thinned, into the fraction; rounded; while the pool has room.
        if (lid == 0u) {
            sSpawnNow = 0u;
            if (sEmits) {
                rngState = hash(row.seed ^ hash(row.frameSerial * 0x85ebca6bu + stepNumber));
                const float rate = max(0.0, sRate + emf(25u) * randN()) * push.eye.w *
                                   emissionDistanceLod(length(emitterWorld - push.eye.xyz));
                if (rate > 0.0) {
                    sAccumulated += rate * step;
                    const int wanted = sAccumulated > 0.0 ? int(round(sAccumulated)) : 0;
                    const uint room = sPool > sLive ? sPool - sLive : 0u;
                    sSpawnNow = min(uint(max(wanted, 0)), room);
                    sAccumulated -= float(sSpawnNow);
                }
            }
        }
        barrier();
        // Into the free records, in their order.
        const uint spawnNow = sSpawnNow;
        uint taken = 0u;
        for (uint c = 0u; c < chunks && taken < spawnNow; ++c) {
            const uint i = c * 64u + lid;
            const uint free = i < capacity && !alive(i) ? 1u : 0u;
            uint inChunk;
            const uint rank = taken + prefixSum(free, inChunk);
            if (free != 0u && rank < spawnNow) spawn(row, i, sSpawned + rank, step);
            taken += inChunk;
        }
        barrier();
        if (lid == 0u) sSpawned += spawnNow;
        // Every particle moves, the newborn too, and the old go.
        for (uint i = lid; i < capacity; i += 64u)
            if (alive(i)) move(i, step);
        barrier();
        ++stepNumber;
    }

    // What is alive, and how far it reaches from its instance.
    if (lid == 0u) { sLive = 0u; sReach = 0u; sVisible = 0u; }
    barrier();
    const bool modelSpace = (flags & kFlagModelSpace) != 0u;
    uint mine = 0u;
    float reach = 0.0;
    for (uint i = lid; i < capacity; i += 64u) {
        if (!alive(i)) continue;
        ++mine;
        const vec3 p = record(1u + i, 0u).xyz;
        const vec3 world = modelSpace ? (sToWorld * vec4(p, 1.0)).xyz : p;
        reach = max(reach, length(world - row.cullCentre.xyz));
    }
    atomicAdd(sLive, mine);
    atomicMax(sReach, floatBitsToUint(reach));
    barrier();
    if (lid == 0u) {
        setRecord(0u, 0u, vec4(uintBitsToFloat(row.tag), sAccumulated, uintBitsToFloat(sPool),
                               uintBitsToFloat(sSpawned)));
        read[rowIndex] = uvec2(sLive, sReach);
    }

    // Seen: its particles as quads, a head, a tail or both each, in the
    // order of their records, up to its draw cap (0x4000 vertices).
    if ((row.flags & kInView) == 0u) return;
    const uint drawFlags = em(9u);
    const bool head = (drawFlags & 1u) != 0u;
    const bool tail = (drawFlags & 2u) != 0u;
    const uint perParticle = head && tail ? 2u : 1u;
    const uint cap = head && tail ? 2048u : 4096u;
    const float twinkleSpeed = emf(12u);
    const float twinklePercent = emf(13u);
    // Which are shown: twinkle hides one while its slot is above the
    // percentage.
    uint shown = 0u;
    if (head || tail) {
        for (uint i = lid; i < capacity; i += 64u) {
            if (!alive(i)) continue;
            const vec4 a = record(1u + i, 0u);
            const uint offset = floatBitsToUint(record(1u + i, 3u).y);
            const float twinkle = twinkleAt(twinkleSlot(twinkleSpeed, a.w, offset));
            if (twinklePercent < 1.0 && twinkle > twinklePercent) continue;
            ++shown;
        }
    }
    atomicAdd(sVisible, shown);
    barrier();
    // Its range of the frame's quads.
    if (lid == 0u) {
        const uint wanted = min(sVisible, cap) * perParticle;
        uint first = wanted > 0u ? atomicAdd(quadsTaken, wanted) : 0u;
        uint count = wanted;
        if (first + wanted > quadRoom) {
            count = first < quadRoom ? quadRoom - first : 0u;
            count -= count % perParticle;
            atomicAdd(quadsShort, wanted - count);
        }
        draws[rowIndex] = uvec4(4u, count, 0u, first);
        sVisible = count / perParticle;
        sQuadFirst = first;
    }
    barrier();
    const uint limit = sVisible;
    const uint quadFirst = sQuadFirst;
    if (limit == 0u) return;

    const float fade = clamp(row.cullCentre.w, 0.0, 1.0);
    const float boneScale = length(sToWorld[0].xyz);
    const uint tiles = (em(3u) & 0xFFFFu) * (em(3u) >> 16u);
    const bool randomCell = tiles > 1u && (flags & kFlagRandomCell) != 0u;
    const uint fblocks = emitterRecord + kEmitterFBlocks;
    uint laid = 0u;
    for (uint c = 0u; c < chunks && laid < limit; ++c) {
        const uint i = c * 64u + lid;
        bool show = i < capacity && alive(i);
        vec4 a = vec4(0.0), b = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0);
        float twinkle = 0.0;
        if (show) {
            a = record(1u + i, 0u);
            b = record(1u + i, 1u);
            r2 = record(1u + i, 2u);
            r3 = record(1u + i, 3u);
            twinkle = twinkleAt(twinkleSlot(twinkleSpeed, a.w, floatBitsToUint(r3.y)));
            show = twinklePercent >= 1.0 || twinkle <= twinklePercent;
        }
        uint inChunk;
        const uint rank = laid + prefixSum(show ? 1u : 0u, inChunk);
        laid += inChunk;
        if (!show || rank >= limit) continue;

        const float age = a.w;
        const float lifeRatio = age / max(b.w, 0.001);
        // Colour and alpha as authored, the alpha times the model's fade.
        const vec3 color = fblock(word(fblocks + 0u), lifeRatio, vec4(1.0)).rgb;
        const float alpha = fblock(word(fblocks + 1u), lifeRatio, vec4(1.0)).x * fade;
        // Half its width and height: the scale block, its own variation and
        // the twinkle, and with flag 0x20 the bone's world scale.
        vec2 halfSize = fblock(word(fblocks + 2u), lifeRatio, vec4(1.0)).xy * r2.xy *
                        (emf(14u) + twinkle * (emf(15u) - emf(14u)));
        if ((flags & kFlagBoneScale) != 0u) halfSize *= boneScale;
        const float spin = r3.x * particleSpin(emf(16u), emf(17u), emf(18u), emf(19u), r2.z, r2.w, age);
        const vec3 position = modelSpace ? (sToWorld * vec4(a.xyz, 1.0)).xyz : a.xyz;
        const vec3 velocity = modelSpace ? mat3(sToWorld) * b.xyz : b.xyz;
        vec3 tailVector = vec3(0.0);
        if (tail) {
            float seconds = emf(8u);
            if ((flags & kFlagClampTail) != 0u) seconds = min(seconds, age);
            tailVector = -velocity * seconds;
        }
        // The head: along its flight (0x4) while it moves; flat in a plane
        // (0x1000) - the world's XY, or the emitter's with 0x10 - turned by
        // its spin; else facing the camera.
        uint shape = kShapeBillboard;
        vec3 axisA = vec3(0.0), axisB = vec3(0.0);
        float headSpin = 0.0;
        if ((flags & kFlagAlongVelocity) != 0u && dot(b.xyz, b.xyz) > kAlongVelocityMinSpeedSq) {
            shape = kShapeAlongVelocity;
            axisA = -velocity;
        } else if ((flags & kFlagPlane) != 0u) {
            vec3 x = vec3(1.0, 0.0, 0.0), y = vec3(0.0, 1.0, 0.0);
            if (modelSpace) {
                x = normalize(sToWorld[1].xyz);
                y = -normalize(sToWorld[0].xyz);
            }
            const float cs = cos(spin), sn = sin(spin);
            shape = kShapePlane;
            axisA = cs * x + sn * y;
            axisB = -sn * x + cs * y;
        } else {
            headSpin = spin;
        }
        // The atlas cell: the head's from its track over its life, or the one
        // it was given (0x10000); the tail's from its own track.
        const float headCell = randomCell ? r3.z : fblockCell(word(fblocks + 3u), lifeRatio, tiles);
        const float tailCell = fblockCell(word(fblocks + 4u), lifeRatio, tiles);
        const vec4 rgba = vec4(color, alpha);
        const uint at = quadFirst + rank * perParticle;
        if (head) writeQuad(at, position, rgba, halfSize, headCell, axisA, axisB, headSpin, shape, row);
        if (tail) writeQuad(head ? at + 1u : at, position, rgba, halfSize, tailCell, tailVector, vec3(0.0), 0.0,
                            kShapeTail, row);
    }
}
