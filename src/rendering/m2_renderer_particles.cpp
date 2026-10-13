#include "rendering/m2_renderer.hpp"
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <unordered_set>
#include "rendering/m2_renderer_internal.h"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
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

// Interpolate an M2 FBlock (particle lifetime curve) at a given life ratio [0..1].
// FBlocks store per-lifetime keyframes for particle color, alpha, and scale.
// NOTE: interpFBlockFloat and interpFBlockVec3 share identical interpolation logic -
// if you fix a bug in one, update the other to match.
float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.floatValues.size() - 1);
            size_t v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[v0], fb.floatValues[v1], frac);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.vec3Values.size() - 1);
            size_t v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], frac);
        }
    }
    return fb.vec3Values.back();
}

std::vector<glm::vec3> M2Renderer::getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const {
    std::vector<glm::vec3> result;
    float maxDistSq = maxDist * maxDist;
    // The same list, for the same reason; see renderM2Particles. A ribbon
    // emitter is a particle emitter as far as this list is concerned.
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (!inst.cachedModel || !inst.cachedModel->isWaterVegetation) continue;
        glm::vec3 diff = inst.position - camPos;
        if (glm::dot(diff, diff) <= maxDistSq) {
            result.push_back(inst.position);
        }
    }
    return result;
}

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (gpu.isInstancePortal) return;

    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);
    std::uniform_int_distribution<int> distTile;

    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const auto& em = gpu.particleEmitters[ei];
        if (!em.enabled) continue;

        float rate = interpFloat(em.emissionRate, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        float life = interpFloat(em.lifespan, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        // What the player asked to see of it, before the floor below. The order
        // is the whole point: thinning first and flooring second lets a low
        // setting take smoke, dust and spell effects down while a candle is
        // pulled back up to the handful of particles that still reads as fire.
        rate *= particleDensity_;

        // A flame reads as a flame only when enough particles are alive at once.
        // Authored rates vary wildly for the same visual intent - a candle asks
        // for 40/s over half a second, CHANDELIER01 for 1/s over six seconds,
        // which sustains a single speck per candle and looks like a bare glow.
        // Steady-state population is rate x lifespan, so floor the rate against
        // the lifespan to hold every fixture at a comparable density. Do not lower
        // this: at 7 the flames disappear entirely, as they do under any change
        // that reduces how many particles are alive or how far they travel. The
        // effect is only visible because a scattering of particles reaches open
        // air, so thinning it drops the whole thing below the threshold.
        if (rate > 0.0f && life > 0.0f &&
            (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire || gpu.isKoboldFlame)) {
            constexpr float kMinLiveParticles = 15.0f;
            rate = std::max(rate, kMinLiveParticles / std::max(life, 0.1f));
        }

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

        inst.emitterAccumulators[ei] += rate * dt;

        while (inst.emitterAccumulators[ei] >= 1.0f && inst.particles.size() < MAX_M2_PARTICLES) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            p.life = 0.0f;
            p.maxLife = life;
            p.tileIndex = 0.0f;

            const auto track = [&](const pipeline::M2AnimationTrack& t) {
                return interpFloat(t, inst.animTime, inst.globalSequenceTime,
                                   inst.currentSequenceIndex, gpu.globalSequenceDurations);
            };

            // Position: the emitter's, anywhere across a plane emitter's area
            // (length along x, width along y, both half extents), carried by
            // its bone.
            glm::vec3 localPos = em.position;
            if (em.emitterType == 1) {
                localPos.x += distN(particleRng_) * track(em.emissionAreaLength);
                localPos.y += distN(particleRng_) * track(em.emissionAreaWidth);
            }
            glm::mat4 boneXform = glm::mat4(1.0f);
            if (em.bone < inst.boneMatrices.size()) {
                boneXform = inst.boneMatrices[em.bone];
            }
            glm::vec3 worldPos = glm::vec3(inst.modelMatrix * boneXform * glm::vec4(localPos, 1.0f));
            p.position = worldPos;

            // Velocity: up the emitter's axis, tilted by up to the vertical
            // range in any direction, at the emission speed give or take its
            // variation. Both ranges are angles in radians. They were added
            // to the direction as lengths, so a horizontal range of 2*pi - a
            // full turn, which leaves "up" up - laid the particles nearly flat:
            // Shadowform's smoke column went out sideways at the feet.
            float speed = track(em.emissionSpeed) *
                          (1.0f + distN(particleRng_) * track(em.speedVariation));
            const float tilt = distN(particleRng_) * track(em.verticalRange);
            const float around = dist01(particleRng_) * glm::two_pi<float>();
            glm::vec3 dir(std::sin(tilt) * std::cos(around), std::sin(tilt) * std::sin(around),
                          std::cos(tilt));
            // A zSource above zero sends them away from that point below the
            // emitter instead - a fountain's spray fanning out from its spout.
            if (const float zSource = track(em.zSource); zSource > 0.0f) {
                const glm::vec3 away = localPos - em.position + glm::vec3(0.0f, 0.0f, zSource);
                if (glm::dot(away, away) > 1e-6f) dir = glm::normalize(away);
            }

            // Transform direction by bone + model orientation (rotation only)
            glm::mat3 rotMat = glm::mat3(inst.modelMatrix * boneXform);
            p.velocity = rotMat * dir * speed;

            // When emission speed is ~0 and bone animation isn't loaded (.anim files),
            // particles pile up at the same position. Give them a drift so they
            // spread outward like a mist/spray effect instead of clustering.
            if (std::abs(speed) < 0.01f) {
                if (gpu.isFireflyEffect) {
                    // Fireflies: gentle random drift in all directions
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.3f
                    );
                } else {
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 1.0f,
                        distN(particleRng_) * 1.0f,
                        -dist01(particleRng_) * 0.5f
                    );
                }
            }

            const uint32_t tilesX = std::max<uint16_t>(em.textureCols, 1);
            const uint32_t tilesY = std::max<uint16_t>(em.textureRows, 1);
            const uint32_t totalTiles = tilesX * tilesY;
            if ((em.flags & kParticleFlagTiled) && totalTiles > 1) {
                if (em.flags & kParticleFlagRandomized) {
                    distTile = std::uniform_int_distribution<int>(0, static_cast<int>(totalTiles - 1));
                    p.tileIndex = static_cast<float>(distTile(particleRng_));
                } else {
                    p.tileIndex = 0.0f;
                }
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
        // Cap accumulator to avoid bursts after lag
        if (inst.emitterAccumulators[ei] > 2.0f) {
            inst.emitterAccumulators[ei] = 0.0f;
        }
    }
}

void M2Renderer::updateParticles(M2Instance& inst, float dt) {
    if (!inst.cachedModel) return;
    const auto& gpu = *inst.cachedModel;

    // Per emitter, not per particle: its gravity and what its drag leaves of
    // a velocity over this step. Gravity is the one the model gives, zero
    // included - a zero used to become 4, which pulled every rising particle
    // back down within half a second: Shadowform's smoke column fell into a
    // swirl at the feet.
    struct EmitterMotion { float gravity = 0.0f; float keep = 1.0f; };
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
        emitterMotion[e] = {.gravity = track(pem.gravity),
                            .keep = std::exp(-std::max(pem.drag, 0.0f) * dt)};
    }

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        if (p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < numEm) {
            const EmitterMotion& motion = emitterMotion[p.emitterIndex];
            p.velocity.z -= motion.gravity * dt;
            p.velocity *= motion.keep;
        }
        p.position += p.velocity * dt;
        i++;
    }
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt, bool emit) {
    if (gpu.isInstancePortal) return;

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
    if (!blended().ribbon || !blended().ribbonAdditive || !ribbonVB_ || !ribbonVBMapped_) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    // The edges carry their own vertices, laid across the emitter's Y axis
    // when they were born (updateRibbons). The strip used to be widened here
    // across the line of sight from a spine of single points, which turned
    // every trail flat to the camera: none of them could twist.
    float* dst     = static_cast<float*>(ribbonVBMapped_);
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    const auto addInstanceRibbons = [&](const M2Instance& inst) {
        if (!inst.cachedModel) return;
        const auto& gpu = *inst.cachedModel;
        if (gpu.isInstancePortal) return;
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
    for (const auto& inst : instances) addInstanceRibbons(inst);
    for (const auto& [id, posed] : posedEmitters_) addInstanceRibbons(posed.inst);

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (const auto& inst : instances) {
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
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!blended().particle || !m2ParticleVB_) return;
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
    if (!m2ParticleVBMapped_) return;
    float* const vbBase = static_cast<float*>(m2ParticleVBMapped_);
    uint32_t vbWritten = 0;
    particleRuns_.clear();
    ParticleGroup* runGroup = nullptr;

    size_t totalParticles = 0;

    // Only the instances that carry emitters, not every instance in the world.
    //
    // particleInstanceIndices_ is built and maintained for exactly this and
    // the update path above already uses it; this pass walked all 66211
    // instances instead, asking each whether its particle vector was empty.
    // That walk was 2.0ms of a 16ms frame - two thirds of the M2 worker,
    // which is the critical path of renderWorld. The smoke pass beside this
    // one does the same kind of work against a flat list and costs 0.003ms.
    const auto addInstanceParticles = [&](const M2Instance& inst) {
        if (inst.particles.empty()) return;
        if (!inst.cachedModel) return;
        const auto& gpu = *inst.cachedModel;
        if (gpu.isInstancePortal) return;


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
        // animFrame depends only on inst.animTime + totalTiles, so it's also
        // emitter-stable within one frame.
        uint32_t cachedAnimFrame = 0;
        float cachedTilesFloat = 1.0f;
        bool cachedIsTiled = false;
        float invAnimMs = 1.0f / 1000.0f;

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
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType), .tilesX = cachedTilesX, .tilesY = cachedTilesY};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

                cachedIsTiled = (cachedEm->flags & kParticleFlagTiled) && cachedTotalTiles > 1;
                if (cachedIsTiled) {
                    float animSeconds = inst.animTime * invAnimMs;
                    cachedAnimFrame = static_cast<uint32_t>(std::floor(animSeconds * cachedTotalTiles))
                                      % cachedTotalTiles;
                    cachedTilesFloat = static_cast<float>(cachedTotalTiles);
                }
            }

            const auto& em = *cachedEm;
            float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f);
            float rawScale = interpFBlockFloat(em.particleScale, lifeRatio);

            if (!gpu.isSpellEffect && !gpu.isFireflyEffect && !gpu.isLanternLike &&
                !gpu.isTorch && !gpu.isBrazierOrFire && !gpu.isKoboldFlame) {
                color = glm::mix(color, glm::vec3(1.0f), 0.7f);
                if (rawScale > 2.0f) alpha *= 0.02f;
                if (cachedBlendType == 3 || cachedBlendType == 4) alpha *= 0.05f;
            }
            // Flame fixtures: the authored curves can leave a particle with
            // effectively no colour or alpha for most of its life. CHANDELIER01
            // ramps scale from zero over a six second life and its candles spend
            // nearly all of that time contributing nothing - and because these
            // draw additively, a near-black particle adds literally nothing to
            // the frame. Floor colour and alpha so a lit fixture always shows
            // flame. Floors only lift the dim end, leaving torches and candles
            // that already read correctly untouched.
            if (gpu.isLanternLike || gpu.isTorch ||
                gpu.isBrazierOrFire || gpu.isKoboldFlame) {
                color = glm::max(color, glm::vec3(0.50f, 0.26f, 0.09f));
                alpha = std::max(alpha, 0.30f);
            }

            float scale = rawScale;
            if (gpu.isSpellEffect) {
                scale = std::max(rawScale * 1.5f, 0.15f);
            } else if (!gpu.isFireflyEffect) {
                scale = std::min(rawScale, 1.5f);
                // Candle flames are authored at a fraction of a unit, which lands
                // sub-pixel at any normal viewing distance - the fixture glows
                // with no visible flame. Small effect-heavy models dodge this by
                // being classified as spell effects (three or more emitters and
                // few vertices) and picking up that path's floor, but a
                // chandelier is a real fixture at 370 vertices and misses the
                // cut, so its five candles rendered as nothing. Give flame
                // fixtures the same floor without making them spell effects,
                // which would also change how they blend.
                if (gpu.isLanternLike || gpu.isTorch ||
                    gpu.isBrazierOrFire || gpu.isKoboldFlame) {
                    scale = std::max(scale, 0.15f);
                }
            }

            if (vbWritten >= MAX_M2_PARTICLE_VERTS) break;
            // A run per stretch of particles sharing a group. The group only
            // changes when the emitter does, and particles from one emitter
            // are adjacent, so this closes a run about once per emitter.
            if (cachedGroup != runGroup) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = vbWritten, .count = 0});
            }
            highestParticleZ = std::max(highestParticleZ, p.position.z);
            widestParticle = std::max(widestParticle, scale);

            float* vd = vbBase + static_cast<size_t>(vbWritten) * 9;
            vd[0] = p.position.x;
            vd[1] = p.position.y;
            vd[2] = p.position.z;
            vd[3] = color.r;
            vd[4] = color.g;
            vd[5] = color.b;
            vd[6] = alpha;
            vd[7] = scale;
            float tileIndex = p.tileIndex;
            if (cachedIsTiled) {
                tileIndex = p.tileIndex + static_cast<float>(cachedAnimFrame);
                while (tileIndex >= cachedTilesFloat) {
                    tileIndex -= cachedTilesFloat;
                }
            }
            vd[8] = tileIndex;
            ++vbWritten;
            ++particleRuns_.back().count;
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
    for (size_t idx : particleInstanceIndices_) {
        if (idx < instances.size()) addInstanceParticles(instances[idx]);
    }
    for (const auto& [id, posed] : posedEmitters_) addInstanceParticles(posed.inst);

    // Periodic diagnostic: spell effect particle count
    {
        static uint32_t spellParticleDiagFrame_ = 0;
        if (++spellParticleDiagFrame_ % 300 == 1) {
            size_t spellPtc = 0;
            for (const auto& inst : instances) {
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
    vkCmdBindVertexBuffers(cmd, 0, 1, &m2ParticleVB_, &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (auto& run : particleRuns_) {
        if (run.count == 0 || !run.group) continue;
        ParticleGroup& group = *run.group;

        uint8_t blendType = group.blendType;
        VkPipeline desiredPipeline = (blendType == 3 || blendType == 4)
            ? blended().particleAdditive : blended().particle;
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

        // Push constants: tileCount + alphaKey
        struct { float tileX, tileY; int alphaKey; } pc = {
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .alphaKey = (blendType == 1) ? 1 : 0
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // The vertices are already in the buffer, at this run's own offset.
        // Both used to be wrong: every group copied to offset zero and drew
        // from vertex zero, so with more than one group up they all drew
        // whatever had been copied last.
        vkCmdDraw(cmd, run.count, 1, run.first, 0);
    }
}

void M2Renderer::renderSmokeParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (smokeParticles.empty() || !blended().smoke || !smokeVB_) return;

    // Build vertex data: pos(3) + lifeRatio(1) + size(1) + isSpark(1) per particle
    size_t count = std::min(smokeParticles.size(), static_cast<size_t>(MAX_SMOKE_PARTICLES));
    float* dst = static_cast<float*>(smokeVBMapped_);
    for (size_t i = 0; i < count; i++) {
        const auto& p = smokeParticles[i];
        *dst++ = p.position.x;
        *dst++ = p.position.y;
        *dst++ = p.position.z;
        *dst++ = p.life / p.maxLife;
        *dst++ = p.size;
        *dst++ = p.isSpark;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blended().smoke);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            smokePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Push constant: screenHeight
    float screenHeight = static_cast<float>(vkCtx_->getSwapchainExtent().height);
    vkCmdPushConstants(cmd, smokePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(float), &screenHeight);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &smokeVB_, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
