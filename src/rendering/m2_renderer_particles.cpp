#include "rendering/m2_renderer.hpp"
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <unordered_set>
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_particle_emission.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <limits>
#include <set>

namespace wowee {
namespace rendering {

// --- M2 Particle Emitter Helpers ---

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                                float globalTime, int seqIdx,
                                const std::vector<uint32_t>& globalSeqDurations) {
    return m2_track::sampleFloat(track, seqIdx, animTime, globalTime,
                                 globalSeqDurations, 0.0f);
}

namespace {

// An FBlock's value at a fraction t of a particle's life, as the client
// takes it (m2FBlockSpan); `none` when the block has no keys.
template<class T>
T sampleFBlock(const std::vector<T>& values, const std::vector<float>& timestamps, float t, T none) {
    if (values.empty()) return none;
    const M2FBlockSpan span = m2FBlockSpan(timestamps, values.size(), t);
    return glm::mix(values[span.from], values[span.to], span.frac);
}

// The emitter's space into the world: the model, then the emitter's bone.
glm::mat4 emitterToWorld(const M2Instance& inst, const pipeline::M2ParticleEmitter& em) {
    if (em.bone < inst.boneMatrices.size()) return inst.modelMatrix * inst.boneMatrices[em.bone];
    return inst.modelMatrix;
}

// Where a particle is in the world. One of a model-space emitter (flag 0x10)
// is kept in its emitter's space and moves with it.
glm::vec3 particleWorldPosition(const M2Instance& inst, const M2ModelGPU& gpu, const M2Particle& p) {
    if (p.emitterIndex < 0 || static_cast<size_t>(p.emitterIndex) >= gpu.particleEmitters.size())
        return p.position;
    const auto& em = gpu.particleEmitters[p.emitterIndex];
    if (!(em.flags & pipeline::kM2EmitterModelSpace)) return p.position;
    return glm::vec3(emitterToWorld(inst, em) * glm::vec4(p.position, 1.0f));
}

} // namespace

float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    return sampleFBlock(fb.floatValues, fb.timestamps, lifeRatio, 1.0f);
}

glm::vec2 M2Renderer::interpFBlockVec2(const pipeline::M2FBlock& fb, float lifeRatio) {
    return sampleFBlock(fb.vec2Values, fb.timestamps, lifeRatio, glm::vec2(1.0f));
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    return sampleFBlock(fb.vec3Values, fb.timestamps, lifeRatio, glm::vec3(1.0f));
}

// An atlas cell track: the interpolated cell rounded to the nearest, kept
// within the atlas. No track is cell 0.
float M2Renderer::interpFBlockCell(const pipeline::M2FBlock& fb, float lifeRatio,
                                   uint32_t cellCount) {
    if (cellCount == 0) return 0.0f;
    const float cell = std::round(sampleFBlock(fb.floatValues, fb.timestamps, lifeRatio, 0.0f));
    return std::clamp(cell, 0.0f, static_cast<float>(cellCount - 1));
}

std::vector<glm::vec3> M2Renderer::getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const {
    std::vector<glm::vec3> result;
    float maxDistSq = maxDist * maxDist;
    // The same list, for the same reason; see renderM2Particles. A ribbon
    // emitter is a particle emitter as far as this list is concerned.
    for (size_t idx : scene_->particleInstanceIndices_) {
        if (idx >= scene_->instances.size()) continue;
        const auto& inst = scene_->instances[idx];
        if (!inst.cachedModel || !inst.cachedModel->isWaterVegetation) continue;
        glm::vec3 diff = inst.position - camPos;
        if (glm::dot(diff, diff) <= maxDistSq) {
            result.push_back(inst.position);
        }
    }
    return result;
}

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {

    const size_t emitterCount = gpu.particleEmitters.size();
    if (inst.emitterAccumulators.size() != emitterCount) {
        inst.emitterAccumulators.resize(emitterCount, 0.0f);
    }
    if (inst.emitterPools.size() != emitterCount) inst.emitterPools.resize(emitterCount, 0u);

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);
    std::uniform_int_distribution<int> distTile;

    // Each emitter's live particles, against its pool.
    std::vector<uint32_t> live(emitterCount, 0u);
    for (const auto& p : inst.particles) {
        if (p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < emitterCount)
            ++live[p.emitterIndex];
    }

    for (size_t ei = 0; ei < emitterCount; ei++) {
        const auto& em = gpu.particleEmitters[ei];
        if (!em.enabled) continue;

        const float authoredRate = interpFloat(em.emissionRate, inst.animTime, inst.globalSequenceTime,
                                               inst.currentSequenceIndex, gpu.globalSequenceDurations);
        float life = interpFloat(em.lifespan, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        // As the client emits (CParticleEmitter2, 0x97d8c0): the rate give or
        // take its variation, thinned by the density setting and by the
        // emitter's distance, into a pool sized from rate and life.
        glm::mat4 emitterBone(1.0f);
        if (em.bone < inst.boneMatrices.size()) emitterBone = inst.boneMatrices[em.bone];
        const glm::vec3 emitterWorld =
            glm::vec3(inst.modelMatrix * emitterBone * glm::vec4(em.position, 1.0f));
        const float rate = m2EmitterRate(authoredRate, em.emissionRateVary, distN(particleRng_),
                                         particleDensity_,
                                         m2EmissionDistanceLod(glm::length(emitterWorld - cachedCamPos_)));
        inst.emitterPools[ei] = std::max(inst.emitterPools[ei],
            m2EmitterPool(authoredRate, em.emissionRateVary, life, em.lifespanVary));

        if (rate <= 0.0f || life <= 0.0f) {
            // Diagnostic: a lamp or flame whose emitter never fires produces a
            // fixture that glows but shows no flame. Report each model once so a
            // default-level log says whether emission is the cause.
            if (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire) {
                static std::unordered_set<std::string> reported;
                if (reported.insert(gpu.name).second) {
                    LOG_WARNING("Flame emitter idle: '", gpu.name, "' emitter=", ei,
                                " rate=", rate, " life=", life,
                                " animTime=", inst.animTime,
                                " seqIdx=", inst.currentSequenceIndex,
                                " gsTime=", inst.globalSequenceTime,
                                " rateSeqs=", em.emissionRate.sequences.size(),
                                " lifeSeqs=", em.lifespan.sequences.size(),
                                " rateGlobalSeq=", em.emissionRate.globalSequence);
                }
            }
            continue;
        }

        // Spawned: the accumulated count rounded, while the pool has room. A
        // full pool stops emission and recycles nothing; what was not spawned
        // stays accumulated.
        inst.emitterAccumulators[ei] += rate * dt;
        const int wanted = m2SpawnCount(inst.emitterAccumulators[ei]);
        for (int n = 0; n < wanted && live[ei] < inst.emitterPools[ei]; ++n, ++live[ei]) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            // Born somewhere within this step, as the client does, so that a
            // frame's particles do not move as one sheet.
            p.life = dist01(particleRng_) * dt;
            p.maxLife = m2ParticleLife(life);
            p.tileIndex = 0.0f;
            p.sizeFactor = m2ScaleVaryFactor(em.scaleVary,
                                             (em.flags & pipeline::kM2EmitterIndependentScaleVary) != 0,
                                             distN(particleRng_), distN(particleRng_));
            p.spinRandBase = distN(particleRng_);
            p.spinRandRate = distN(particleRng_);
            p.spinSign = (em.flags & pipeline::kM2EmitterRandomSpin) && dist01(particleRng_) < 0.5f
                ? -1.0f : 1.0f;
            p.twinkleOffset = static_cast<uint32_t>(particleRng_()) & (kM2TwinkleSlots - 1);

            const auto track = [&](const pipeline::M2AnimationTrack& t) {
                return interpFloat(t, inst.animTime, inst.globalSequenceTime,
                                   inst.currentSequenceIndex, gpu.globalSequenceDurations);
            };

            // Where it starts and which way it goes, in the emitter's space
            // (section 4.5). A plane spreads them over its area and sends them
            // into a cone about +Z; a sphere places them between its two radii
            // and sends them out from the centre, or up with flag 0x100. A
            // zSource sends either away from that point on the emitter's axis.
            const float speed = track(em.emissionSpeed) *
                                (1.0f + distN(particleRng_) * track(em.speedVariation));
            const float verticalRange = track(em.verticalRange);
            const float horizontalRange = track(em.horizontalRange);
            glm::vec3 offset(0.0f);
            glm::vec3 dir(0.0f, 0.0f, 1.0f);
            if (em.emitterType == 2) {
                offset = m2SphereSpawnOffset(track(em.emissionAreaLength), track(em.emissionAreaWidth),
                                             dist01(particleRng_),
                                             distN(particleRng_) * verticalRange,
                                             distN(particleRng_) * horizontalRange);
                if (!(em.flags & pipeline::kM2EmitterSphereUp)) {
                    const float len = glm::length(offset);
                    dir = len > 0.0f ? offset / len : glm::vec3(0.0f, 0.0f, 1.0f);
                }
            } else {
                if (em.emitterType == 3) {
                    // Spawning along the spline is not done here: they leave
                    // from the emitter's point.
                    static std::unordered_set<std::string> saidSpline;
                    if (saidSpline.insert(gpu.name).second) {
                        LOG_WARNING("Spline particle emitter ", ei, " of '", gpu.name,
                                    "' emits from its origin; spline spawning is not implemented");
                    }
                }
                if (em.emitterType == 1) {
                    offset = m2PlaneSpawnOffset(track(em.emissionAreaLength), track(em.emissionAreaWidth),
                                                distN(particleRng_), distN(particleRng_));
                }
                dir = m2ConeDirection(distN(particleRng_) * verticalRange,
                                      distN(particleRng_) * horizontalRange);
            }
            if (const float zSource = track(em.zSource); zSource != 0.0f) {
                dir = m2ZSourceDirection(offset, zSource);
            }
            // From the emitter's space into its bone's.
            const glm::vec3 localPos = em.position + m2EmitterToBone(offset);
            dir = m2EmitterToBone(dir);

            // Into the world through the emitter's bone, scale included, or
            // left in the bone's space to be carried by it (flag 0x10).
            glm::mat4 boneXform = glm::mat4(1.0f);
            if (em.bone < inst.boneMatrices.size()) {
                boneXform = inst.boneMatrices[em.bone];
            }
            if (em.flags & pipeline::kM2EmitterModelSpace) {
                p.position = localPos;
                p.velocity = dir * speed;
            } else {
                const glm::mat4 emitterToWorld = inst.modelMatrix * boneXform;
                p.position = glm::vec3(emitterToWorld * glm::vec4(localPos, 1.0f));
                p.velocity = glm::mat3(emitterToWorld) * dir * speed;
            }
            const glm::mat3 rotMat = (em.flags & pipeline::kM2EmitterModelSpace)
                ? glm::mat3(1.0f) : glm::mat3(inst.modelMatrix * boneXform);

            // Fireflies are moved by bone animation from .anim files that are
            // not loaded, so an emitter that stands still gives them a drift of
            // their own. Anything else that stands still stays where it is put:
            // a glow is authored at speed ~0 to sit on its fixture. Given the
            // same drift, the halo on a Dalaran sconce's candle flew off at a
            // yard a second and sank, and the candle showed no light at all.
            if (std::abs(speed) < 0.01f && gpu.isFireflyEffect) {
                p.velocity = rotMat * glm::vec3(
                    distN(particleRng_) * 0.6f,
                    distN(particleRng_) * 0.6f,
                    distN(particleRng_) * 0.3f
                );
            }

            const uint32_t tilesX = std::max<uint16_t>(em.textureCols, 1);
            const uint32_t tilesY = std::max<uint16_t>(em.textureRows, 1);
            const uint32_t totalTiles = tilesX * tilesY;
            if ((em.flags & pipeline::kM2EmitterRandomCell) && totalTiles > 1) {
                distTile = std::uniform_int_distribution<int>(0, static_cast<int>(totalTiles - 1));
                p.tileIndex = static_cast<float>(distTile(particleRng_));
            }

            inst.particles.push_back(p);

            // Diagnostic: log first particle birth per spell effect instance
            if (gpu.isSpellEffect && inst.particles.size() == 1) {
                LOG_INFO("SpellEffect: first particle for '", gpu.name,
                         "' pos=(", p.position.x, ",", p.position.y, ",", p.position.z,
                         ") rate=", rate, " life=", life,
                         " bone=", em.bone, " boneCount=", inst.boneMatrices.size(),
                         " globalSeqs=", gpu.globalSequenceDurations.size());
            }
        }
    }
}

void M2Renderer::updateParticles(M2Instance& inst, float dt) {
    if (!inst.cachedModel) return;
    const auto& gpu = *inst.cachedModel;

    // Per emitter, not per particle: its gravity, as the model gives it, zero
    // included - a zero used to become 4, which pulled every rising particle
    // back down within half a second: Shadowform's smoke column fell into a
    // swirl at the feet.
    struct EmitterMotion { float gravity = 0.0f; };
    constexpr size_t kMaxStackEmitters = 16;
    EmitterMotion emitterMotionStack[kMaxStackEmitters];
    std::vector<EmitterMotion> emitterMotionHeap;
    const size_t numEm = gpu.particleEmitters.size();
    EmitterMotion* emitterMotion = emitterMotionStack;
    if (numEm > kMaxStackEmitters) {
        emitterMotionHeap.resize(numEm);
        emitterMotion = emitterMotionHeap.data();
    }
    for (size_t e = 0; e < numEm; ++e) {
        const auto& pem = gpu.particleEmitters[e];
        const auto track = [&](const pipeline::M2AnimationTrack& t) {
            return interpFloat(t, inst.animTime, inst.globalSequenceTime,
                               inst.currentSequenceIndex, gpu.globalSequenceDurations);
        };
        emitterMotion[e] = {.gravity = track(pem.gravity)};
    }

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        // A particle whose emitter the model no longer has goes with it.
        const bool orphan = p.emitterIndex < 0 || static_cast<size_t>(p.emitterIndex) >= numEm;
        if (!orphan) {
            const auto& em = gpu.particleEmitters[p.emitterIndex];
            m2ParticleStep(p.position, p.velocity, p.life, dt, emitterMotion[p.emitterIndex].gravity,
                           em.drag, em.wind, em.windTime);
        }
        p.life += dt;
        if (orphan || p.life >= p.maxLife) {
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        i++;
    }

    float reachSq = 0.0f;
    for (const auto& p : inst.particles) {
        const glm::vec3 d = particleWorldPosition(inst, gpu, p) - inst.cachedCullCenter;
        reachSq = std::max(reachSq, glm::dot(d, d));
    }
    inst.particleReach = std::sqrt(reachSq);
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt, bool emit) {

    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;

    if (inst.ribbons.size() != emitters.size()) inst.ribbons.resize(emitters.size());

    // As the original client's CRibbonEmitter does it (SetPos, Update and
    // InterpEdge; see Deamon87/WebWowViewerCpp, CRibbonEmitter.cpp, which is
    // that client's code). What it does differently from what this did:
    //  - An edge is laid across the emitter's own Y axis, not across the line
    //    of sight, and keeps that orientation. Trails from a turning bone
    //    twist; the phoenix's wing trails are flat bands winding round.
    //  - New edges follow the curve from the last position to this one, bent
    //    by the emitter's Z axis at both ends, so a fast bone leaves a smooth
    //    arc rather than a polygon with a corner per frame.
    //  - Gravity moves an edge by g * age^2 (z += (2g*age + g*dt)*dt), up for
    //    a negative g. It was z -= g*dt^2/2 per frame - nothing, the wrong
    //    way.
    //  - The tracks are sampled at the animation's time, defaulting to 1, not
    //    read from their first key; an edge's alpha does not fade with age,
    //    the texture does that along u = age / lifetime.
    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& rs = inst.ribbons[ri];

        const float edgesPerSecond = std::max(std::ceil(em.edgesPerSecond), 1.0f);
        const float lifetime = std::max(em.edgeLifetime, 0.25f);
        const size_t capacity = static_cast<size_t>(std::ceil(lifetime * edgesPerSecond)) + 2;

        const auto sample = [&](const pipeline::M2AnimationTrack& track) {
            return m2_track::sampleFloat(track, inst.currentSequenceIndex, inst.animTime,
                                         inst.globalSequenceTime, gpu.globalSequenceDurations, 1.0f);
        };
        const bool visible = sample(em.visibilityTrack) > 0.5f;
        const float above = sample(em.heightAboveTrack);
        const float below = sample(em.heightBelowTrack);
        const float alpha = sample(em.alphaTrack);
        const glm::vec3 color = m2_track::sampleVec3(em.colorTrack, inst.currentSequenceIndex,
            inst.animTime, inst.globalSequenceTime, gpu.globalSequenceDurations, glm::vec3(1.0f));

        // The emitter's frame: model, bone, then its own offset on the bone.
        const glm::mat4 bone = em.bone < inst.boneMatrices.size()
            ? inst.boneMatrices[em.bone] : glm::mat4(1.0f);
        const glm::mat4 frame = inst.modelMatrix * bone * glm::translate(glm::mat4(1.0f), em.position);
        const glm::vec3 pos(frame[3]);
        const glm::vec3 dir(frame[2]);
        const glm::vec3 vertical(frame[1]);
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) continue;

        const bool laying = emit && visible;
        // The first update lays one edge whatever the step, as the client's
        // does, so a trail starts at once.
        const bool first = laying && !rs.posSet;
        if (first) {
            rs.lastPos = pos;
            rs.lastDir = dir;
            rs.lastVertical = vertical;
            rs.startTime = 0.0f;
            rs.posSet = true;
        }
        float step = first ? 1.0f / edgesPerSecond + 0.0001f : dt;
        step = std::clamp(step, 0.0f, lifetime);

        while (!rs.edges.empty() && rs.edges.front().age + step > lifetime) rs.edges.pop_front();

        if (laying) {
            const float scale = glm::length(rs.lastPos - pos);
            const glm::vec3 below0 = rs.lastPos - below * rs.lastVertical;
            const glm::vec3 below1 = pos - below * vertical;
            const glm::vec3 above0 = rs.lastPos + above * rs.lastVertical;
            const glm::vec3 above1 = pos + above * vertical;
            const glm::vec3 lastDirScaled = scale * rs.lastDir;
            const glm::vec3 dirScaled = scale * dir;
            const auto edgeAt = [&](float t, float age) {
                M2Instance::RibbonEdge e;
                e.below = (below1 - (1.0f - t) * dirScaled) * t + (t * lastDirScaled + below0) * (1.0f - t);
                e.above = (above1 - (1.0f - t) * dirScaled) * t + (t * lastDirScaled + above0) * (1.0f - t);
                e.color = color;
                e.alpha = alpha;
                e.age = age;
                return e;
            };

            const float end = step * edgesPerSecond + rs.startTime;
            if (end >= 1.0f) {
                // Born across this step: the earliest is the oldest, and
                // the ageing below brings each to the time since its birth.
                const float ooDenom = 1.0f / (end - rs.startTime);
                const int count = static_cast<int>(std::max(std::floor(end - 1.0f), 0.0f)) + 1;
                for (int i = 1; i <= count; ++i) {
                    const float t = (static_cast<float>(i) - rs.startTime) * ooDenom;
                    rs.edges.push_back(edgeAt(t, -t * step));
                    if (rs.edges.size() >= capacity) rs.edges.pop_front();
                }
                if (gpu.isSpellEffect && rs.edges.size() == static_cast<size_t>(count)) {
                    LOG_INFO("SpellEffect: ribbon edge[0] for '", gpu.name, "' emitter=", ri,
                             " pos=(", pos.x, ",", pos.y, ",", pos.z, ") above=", above,
                             " below=", below, " eps=", edgesPerSecond,
                             " edgeLife=", lifetime, " bone=", em.bone);
                }
            }
            rs.startTime = end - std::floor(end);
            rs.head = edgeAt(1.0f, 0.0f);
            rs.hasHead = true;

            rs.lastPos = pos;
            rs.lastDir = dir;
            rs.lastVertical = vertical;
        } else {
            rs.hasHead = false;
        }

        for (auto& e : rs.edges) {
            const float rise = (2.0f * em.gravity * e.age + em.gravity * step) * step;
            e.below.z += rise;
            e.above.z += rise;
            e.age += step;
        }
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering
// ---------------------------------------------------------------------------
void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    const ::VkBuffer ribbonBuffer = frameBuffer(ribbonVB_);
    if (!blended().ribbon || !blended().ribbonAdditive || !ribbonBuffer) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    // The edges carry their own vertices, laid across the emitter's Y axis
    // when they were born (updateRibbons). The strip used to be widened here
    // across the line of sight from a spine of single points, which turned
    // every trail flat to the camera: none of them could twist.
    float* dst     = static_cast<float*>(frameMapped(ribbonVB_));
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    const auto addInstanceRibbons = [&](const M2Instance& inst) {
        if (!inst.cachedModel || inst.hidden) return;
        const auto& gpu = *inst.cachedModel;
        if (gpu.ribbonEmitters.empty()) return;

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
            if (ri >= inst.ribbons.size()) continue;
            const auto& rs = inst.ribbons[ri];
            const size_t edgeCount = rs.edges.size() + (rs.hasHead ? 1 : 0);
            if (edgeCount < 2) continue;

            const auto& em = gpu.ribbonEmitters[ri];

            // Select blend pipeline based on material blend mode
            const bool additive = ri < gpu.ribbonBlendModes.size() && gpu.ribbonBlendModes[ri] >= 3;
            VkPipeline pipe = additive ? blended().ribbonAdditive : blended().ribbon;

            // Descriptor set for texture
            VkDescriptorSet texSet = (ri < gpu.ribbonTexSets.size())
                                     ? gpu.ribbonTexSets[ri] : VK_NULL_HANDLE;
            if (!texSet) {
                if (gpu.isSpellEffect) {
                    static bool ribbonTexWarn = false;
                    if (!ribbonTexWarn) {
                        LOG_WARNING("SpellEffect: ribbon[", ri, "] for '", gpu.name,
                                    "' has null texSet - descriptor pool may be exhausted");
                        ribbonTexWarn = true;
                    }
                }
                continue;
            }

            uint32_t firstVert = static_cast<uint32_t>(written);

            // A triangle strip, oldest edge first and the head last: below
            // then above per edge. u runs with the edge's age over its
            // lifetime, v from below (0) to above (1); the texture fades the
            // trail, not the vertex alpha.
            const float lifetime = std::max(em.edgeLifetime, 0.25f);
            const auto put = [&](const glm::vec3& p, const M2Instance::RibbonEdge& e, float u, float v) {
                float* vd = dst + written * 9;
                vd[0] = p.x; vd[1] = p.y; vd[2] = p.z;
                vd[3] = e.color.r; vd[4] = e.color.g; vd[5] = e.color.b;
                vd[6] = e.alpha;
                vd[7] = u; vd[8] = v;
                ++written;
            };
            const auto putEdge = [&](const M2Instance::RibbonEdge& e) {
                if (written + 2 > MAX_RIBBON_VERTS) {
                    static bool saidFull = false;
                    if (!saidFull) {
                        saidFull = true;
                        LOG_WARNING("M2 ribbons: ", MAX_RIBBON_VERTS, " vertices in view are not enough;"
                                    " the rest of the trails this frame are cut short, first on '",
                                    gpu.name, "'");
                    }
                    return;
                }
                const float u = std::max(e.age, 0.0f) / lifetime;
                put(e.below, e, u, 0.0f);
                put(e.above, e, u, 1.0f);
            };
            for (const auto& e : rs.edges) putEdge(e);
            if (rs.hasHead) putEdge(rs.head);

            uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({.texSet = texSet, .pipeline = pipe, .firstVertex = firstVert, .vertexCount = vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    };
    for (const auto& inst : scene_->instances) addInstanceRibbons(inst);
    for (const auto& [id, posed] : scene_->posedEmitters_) addInstanceRibbons(posed.inst);

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (const auto& inst : scene_->instances) {
                if (!inst.cachedModel || !inst.cachedModel->isSpellEffect) continue;
                for (const auto& ribbon : inst.ribbons) {
                    if (ribbon.edges.size() >= 2) {
                        spellRibbonDraws++;
                        spellRibbonVerts += ribbon.edges.size() * 2;
                    }
                }
            }
            if (spellRibbonDraws > 0 || !draws.empty()) {
                LOG_INFO("SpellEffect: ", spellRibbonDraws, " spell ribbon strips (",
                         spellRibbonVerts, " verts), total draws=", draws.size(),
                         " written=", written);
            }
        }
    }

    if (draws.empty() || written == 0) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {.x = 0, .y = 0};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : draws) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonBuffer, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    const ::VkBuffer particleBuffer = frameBuffer(m2ParticleVB_);
    if (!particleBuffer) return;
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return;

    // Collect all particles from all instances, grouped by texture+blend.
    // Reuse persistent map - keep the bucket structure, drop last frame's set.
    for (auto& [k, g] : particleGroups_) {
        g.preAllocSet = VK_NULL_HANDLE;
    }
    auto& groups = particleGroups_;

    // Written straight into the mapped buffer rather than accumulated into a
    // vector per group and copied in afterwards; see ParticleRun.
    auto* const vbBase = static_cast<M2ParticleQuad*>(frameMapped(m2ParticleVB_));
    uint32_t vbWritten = 0;
    particleRuns_.clear();
    ParticleGroup* runGroup = nullptr;

    size_t totalParticles = 0;
    size_t droppedForSpace = 0;
    Frustum viewFrustum;
    viewFrustum.extractFromMatrix(cachedViewProjection_);

    // Only the instances that carry emitters, not every instance in the world.
    //
    // particleInstanceIndices_ is built and maintained for exactly this and
    // the update path above already uses it; this pass walked all 66211
    // instances instead, asking each whether its particle vector was empty.
    // That walk was 2.0ms of a 16ms frame - two thirds of the M2 worker,
    // which is the critical path of renderWorld.
    const auto addInstanceParticles = [&](const M2Instance& inst) {
        if (inst.particles.empty()) return;
        if (!inst.cachedModel) return;
        const auto& gpu = *inst.cachedModel;
        // Each emitter draws at most m2EmitterDrawCap particles; the rest are
        // simulated and not drawn, as in the client.
        drawnPerEmitter_.assign(gpu.particleEmitters.size(), 0u);


        // Cache the last emitter's per-emitter state so adjacent particles
        // sharing an emitter (the common case - particles from one source
        // cluster together) skip the texture/key/map-lookup work entirely.
        int lastEmitterIdx = -1;
        VkTexture* cachedTex = nullptr;
        uint16_t cachedTilesX = 1, cachedTilesY = 1;
        uint32_t cachedTotalTiles = 1;
        uint16_t cachedBlendType = 0;
        const pipeline::M2ParticleEmitter* cachedEm = nullptr;
        ParticleGroup* cachedGroup = nullptr;
        bool cachedRandomCell = false;
        bool cachedLit = false;
        // A lit particle takes its model's lights (render spec, lit particles).
        const LocalLightSet instLights = frameLights_ && !frameLights_->empty()
            ? pickLocalLights(*frameLights_, glm::vec3(inst.modelMatrix[3]), 0.0f)
            : kNoLocalLights;

        // How far this instance's particles actually reach, against how far
        // the model says it extends. A fire twice the height of the hut behind
        // it is either particles outliving their authored lifespan, flying at
        // the wrong speed, or drawn at the wrong size - and none of those says
        // which model it is. This does.
        float highestParticleZ = -std::numeric_limits<float>::max();
        float widestParticle = 0.0f;

        for (const auto& p : inst.particles) {
            if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(gpu.particleEmitters.size())) continue;

            if (p.emitterIndex != lastEmitterIdx) {
                lastEmitterIdx = p.emitterIndex;
                cachedEm = &gpu.particleEmitters[p.emitterIndex];

                cachedTex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                    cachedTex = gpu.particleTextures[p.emitterIndex];
                }
                cachedTilesX = std::max<uint16_t>(cachedEm->textureCols, 1);
                cachedTilesY = std::max<uint16_t>(cachedEm->textureRows, 1);
                cachedTotalTiles = static_cast<uint32_t>(cachedTilesX) *
                                   static_cast<uint32_t>(cachedTilesY);
                cachedBlendType = cachedEm->blendingType;
                const bool fogged = !(cachedEm->flags & pipeline::kM2EmitterUnfogged);
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType), .tilesX = cachedTilesX, .tilesY = cachedTilesY, .fogged = fogged};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->fogged = fogged;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

                cachedRandomCell = cachedTotalTiles > 1 &&
                                   (cachedEm->flags & pipeline::kM2EmitterRandomCell);
                // Lit unless flag 0x1, and never Mod or Mod2x (0xa45374).
                cachedLit = !(cachedEm->flags & pipeline::kM2EmitterUnlit) &&
                            !m2BlendIsModulate(cachedEm->blendingType);
            }

            const auto& em = *cachedEm;
            if (!em.drawsHead && !em.drawsTail) continue;
            const float lifeRatio = p.life / std::max(p.maxLife, 0.001f);

            // Twinkle: hidden while its slot in the table is above the
            // percentage, and sized by the slot.
            const float twinkle = m2TwinkleTable()[m2TwinkleSlot(em.twinkleSpeed, p.life, p.twinkleOffset)];
            if (em.twinklePercent < 1.0f && twinkle > em.twinklePercent) continue;

            // Colour and alpha as authored, the alpha times the model's fade.
            // Flame fixtures were floored at 0.3 alpha and a dim orange, and
            // every other doodad's particles washed 70% to white.
            const glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            const float alpha = interpFBlockFloat(em.particleAlpha, lifeRatio) *
                                std::clamp(inst.fade, 0.0f, 1.0f);

            // Half the particle's width and height, in world units (the
            // client's corners are +-1 times the scale): the scale block, the
            // particle's own variation and the twinkle, and the bone's world
            // scale with flag 0x20. A doodad's particles were capped at 1.5,
            // and spell effects' and flame fixtures' floored at 0.15.
            const glm::mat4 toWorld = emitterToWorld(inst, em);
            glm::vec2 halfSize = interpFBlockVec2(em.particleScale, lifeRatio) * p.sizeFactor *
                (em.twinkleScaleMin + twinkle * (em.twinkleScaleMax - em.twinkleScaleMin));
            if (em.flags & pipeline::kM2EmitterBoneScale) {
                halfSize *= glm::length(glm::vec3(toWorld[0]));
            }
            const float spin = p.spinSign * m2ParticleSpin(em.baseSpin, em.baseSpinVary, em.spin,
                                                           em.spinVary, p.spinRandBase,
                                                           p.spinRandRate, p.life);

            // Where it is and how it flies, in the world.
            const bool modelSpace = (em.flags & pipeline::kM2EmitterModelSpace) != 0;
            const glm::vec3 position = modelSpace
                ? glm::vec3(toWorld * glm::vec4(p.position, 1.0f)) : p.position;
            const glm::vec3 velocity = modelSpace ? glm::mat3(toWorld) * p.velocity : p.velocity;
            glm::vec3 tail(0.0f);
            if (em.drawsTail) {
                float seconds = em.tailLength;
                if (em.flags & pipeline::kM2EmitterClampTailToAge) seconds = std::min(seconds, p.life);
                tail = -velocity * seconds;
            }

            // The head's layout: stretched along its flight (flag 0x4) while
            // it moves; else flat in a plane (0x1000) - the world's XY plane,
            // or the emitter's with 0x10 - turned by the spin; else facing
            // the camera.
            M2ParticleQuad head{.shape = static_cast<uint32_t>(M2ParticleShape::Billboard)};
            if ((em.flags & pipeline::kM2EmitterAlongVelocity) &&
                glm::dot(p.velocity, p.velocity) > kM2AlongVelocityMinSpeedSq) {
                head.shape = static_cast<uint32_t>(M2ParticleShape::AlongVelocity);
                head.axisA = -velocity;
            } else if (em.flags & pipeline::kM2EmitterPlane) {
                // The emitter's X and Y, bone scale divided out: bone +Y and
                // bone -X (m2EmitterToBone).
                glm::vec3 x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f);
                if (modelSpace) {
                    x = glm::normalize(glm::vec3(toWorld[1]));
                    y = -glm::normalize(glm::vec3(toWorld[0]));
                }
                const float c = std::cos(spin), s = std::sin(spin);
                head.shape = static_cast<uint32_t>(M2ParticleShape::Plane);
                head.axisA = c * x + s * y;
                head.axisB = -s * x + c * y;
            } else {
                head.spin = spin;
            }
            const int quads = (em.drawsHead && em.drawsTail) ? 2 : 1;
            if (drawnPerEmitter_[p.emitterIndex]++ >= m2EmitterDrawCap(em.drawsHead, em.drawsTail)) {
                continue;
            }

            if (vbWritten + quads > MAX_M2_PARTICLE_QUADS) {
                ++droppedForSpace;
                continue;
            }
            // A run per stretch of particles sharing a group. The group only
            // changes when the emitter does, and particles from one emitter
            // are adjacent, so this closes a run about once per emitter.
            if (cachedGroup != runGroup) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = vbWritten, .count = 0});
            }
            highestParticleZ = std::max(highestParticleZ, position.z);
            widestParticle = std::max(widestParticle, std::max(halfSize.x, halfSize.y));

            // The atlas cell: the head's from its track over the particle's
            // life, or the one it was given at spawn (flag 0x10000); the
            // tail's always from its own track. Every particle of a tiled
            // emitter used to step through the whole atlas on the model's
            // clock, whatever its age.
            const float headCell = cachedRandomCell ? p.tileIndex
                : interpFBlockCell(em.headCellTrack, lifeRatio, cachedTotalTiles);
            const float tailCell = interpFBlockCell(em.tailCellTrack, lifeRatio, cachedTotalTiles);
            for (int q = 0; q < quads; ++q) {
                // With both, the first quad is the head and the second the tail.
                const bool isHead = quads == 2 ? q == 0 : em.drawsHead;
                M2ParticleQuad quad = isHead ? head : M2ParticleQuad{
                    .axisA = tail,
                    .shape = static_cast<uint32_t>(M2ParticleShape::Tail),
                };
                quad.position = position;
                quad.color = glm::vec4(color, alpha);
                quad.halfSize = halfSize;
                quad.cell = isHead ? headCell : tailCell;
                quad.lit = cachedLit ? 1u : 0u;
                quad.light = inst.interiorLight;
                quad.localLights = instLights;
                vbBase[vbWritten] = quad;
                ++vbWritten;
                ++particleRuns_.back().count;
            }
            totalParticles++;
        }

        // Said once per model, on an absolute reach rather than a ratio
        // against the model's own bounds. A bonfire's authored box is 25 to 32
        // yards tall - it has to hold the flame's full extent - so a ratio
        // test can never trip on the one model anybody is complaining about.
        // Nothing that stands on the ground should be throwing particles eight
        // yards into the air.
        if (highestParticleZ > -std::numeric_limits<float>::max()) {
            const float reach = highestParticleZ - inst.position.z;
            if (reach > 8.0f) {
                static std::set<std::string> saidTall;
                if (saidTall.insert(gpu.name).second) {
                    LOG_WARNING("Particles reach ", reach, " yd above '", gpu.name,
                                "' (model bound ", gpu.boundMax.z, " yd, scale ", inst.scale,
                                ", ", gpu.particleEmitters.size(), " emitters, ",
                                inst.particles.size(), " live, largest ", widestParticle, " yd)");
                }
            }
        }
    };
    // Only what is in view. Every emitter within the render distance used to
    // go in, behind the camera as well: Dalaran's two mage portals alone hold
    // 7200 particles, its sconces and candles thousands more, and the buffer
    // filled before a Shadowform priest's fire - created last, so listed last
    // - got any of it. The sphere is the model's and its particles' reach,
    // with room for a particle's own size.
    constexpr float kParticleSizeMargin = 2.0f;
    for (size_t idx : scene_->particleInstanceIndices_) {
        if (idx >= scene_->instances.size()) continue;
        const M2Instance& inst = scene_->instances[idx];
        if (inst.particles.empty()) continue;
        const float radius = std::max(inst.cachedPaddedRadius, inst.particleReach) + kParticleSizeMargin;
        if (!viewFrustum.intersectsSphere(inst.cachedCullCenter, radius)) continue;
        addInstanceParticles(inst);
    }
    for (const auto& [id, posed] : scene_->posedEmitters_) addInstanceParticles(posed.inst);
    if (droppedForSpace > 0) {
        static core::LogBudget overflowBudget(8, "M2 particle buffer overflows");
        if (overflowBudget.take()) {
            LOG_WARNING("M2 particle buffer full: ", droppedForSpace, " particles in view not drawn (",
                        MAX_M2_PARTICLE_QUADS, " quads); the effects listed last lose them");
        }
    }

    // Periodic diagnostic: spell effect particle count
    {
        static uint32_t spellParticleDiagFrame_ = 0;
        if (++spellParticleDiagFrame_ % 300 == 1) {
            size_t spellPtc = 0;
            for (const auto& inst : scene_->instances) {
                if (inst.cachedModel && inst.cachedModel->isSpellEffect)
                    spellPtc += inst.particles.size();
            }
            if (spellPtc > 0) {
                LOG_INFO("SpellEffect: rendering ", spellPtc, " spell particles (",
                         totalParticles, " total)");
            }
        }
    }

    if (totalParticles == 0) return;

    // Bind per-frame set (set 0) for particle pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &particleBuffer, &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (auto& run : particleRuns_) {
        if (run.count == 0 || !run.group) continue;
        ParticleGroup& group = *run.group;

        const M2BlendPipeline blendPipe = m2BlendPipeline(group.blendType);
        VkPipeline desiredPipeline = blended().particle[static_cast<size_t>(blendPipe)];
        if (!desiredPipeline) continue;
        if (desiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
            currentPipeline = desiredPipeline;
        }

        // Use pre-allocated stable descriptor set; fall back to per-frame alloc only if unavailable
        VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
            // Fallback: allocate per-frame (pool exhaustion risk - should not happen in practice)
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &texSet) == VK_SUCCESS) {
                VkTexture* tex = (group.texture && group.texture->isValid())
                    ? group.texture : whiteTexture_.get();
                if (!tex || !tex->isValid()) continue;
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = texSet;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
            }
        }
        if (texSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    particlePipelineLayout_, 1, 1, &texSet, 0, nullptr);
        }

        const M2ParticlePush pc{
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .alphaRef = m2AlphaRef(blendPipe),
            .fogMode = static_cast<int32_t>(group.fogged ? m2FogMode(blendPipe) : M2FogMode::None),
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // The quads are already in the buffer, at this run's own offset.
        // Both used to be wrong: every group copied to offset zero and drew
        // from vertex zero, so with more than one group up they all drew
        // whatever had been copied last.
        vkCmdDraw(cmd, 4, run.count, 0, run.first);
    }
}

} // namespace rendering
} // namespace wowee
