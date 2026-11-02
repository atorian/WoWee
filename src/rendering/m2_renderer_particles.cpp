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
#include <cstdlib>
#include <random>
#include <limits>
#include <set>
#include <tuple>

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

// An emitter's own seed, fixed for its life: its particles' randomness is
// drawn from it and their number (m2_particles.comp).
uint32_t emitterSeed(uint32_t owner, uint32_t emitter, bool posed) {
    uint32_t x = owner * 0x9e3779b9u ^ (emitter + 1u) * 0x85ebca6bu ^ (posed ? 0xc2b2ae35u : 0u);
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    return x;
}

// Nearest first. Equally far ones by where they stand, then by model, so the
// order is the same whatever order the instances were loaded in: blended
// particles drawn in load order came out differently from one run of the
// same scene to the next.
template <typename Emitter>
void sortNearestFirst(std::vector<Emitter>& emitters) {
    std::sort(emitters.begin(), emitters.end(), [](const Emitter& a, const Emitter& b) {
        if (a.distSq != b.distSq) return a.distSq < b.distSq;
        const glm::vec3 p(a.inst->modelMatrix[3]);
        const glm::vec3 q(b.inst->modelMatrix[3]);
        if (p != q) return std::tie(p.x, p.y, p.z) < std::tie(q.x, q.y, q.z);
        return a.inst->cachedModel->name < b.inst->cachedModel->name;
    });
}

// The blocks of `items` starting at `starts` in reverse order, each block as
// it was.
template <typename T>
void reverseBlocks(std::vector<T>& items, const std::vector<size_t>& starts, std::vector<T>& scratch) {
    scratch.clear();
    size_t end = items.size();
    for (size_t b = starts.size(); b-- > 0;) {
        scratch.insert(scratch.end(), items.begin() + static_cast<std::ptrdiff_t>(starts[b]),
                       items.begin() + static_cast<std::ptrdiff_t>(end));
        end = starts[b];
    }
    items.swap(scratch);
}

} // namespace

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

bool M2Renderer::placeParticles(M2Instance& inst) {
    const M2ModelGPU* model = inst.cachedModel;
    if (!model || !model->emitters.placed()) return false;
    const size_t emitters = model->emitters.capacities.size();
    if (inst.particleRanges.size() != emitters) inst.particleRanges.resize(emitters);
    bool any = false;
    for (size_t e = 0; e < emitters; ++e) {
        ParticlePool::Range& range = inst.particleRanges[e];
        if (!range.placed() && model->emitters.capacities[e] > 0)
            range = particles_.place(model->emitters.capacities[e], model->name.c_str());
        any = any || range.placed();
    }
    return any;
}

void M2Renderer::releaseParticles(M2Instance& inst) {
    for (const ParticlePool::Range& range : inst.particleRanges) particles_.release(range);
    inst.particleRanges.clear();
    inst.liveParticles = 0;
    inst.particleReach = 0.0f;
}

void M2Renderer::takeParticleReads() {
    // A read holds a row of one frame, each emitter of an instance a row of
    // its own: the first row of a read starts the instance's count over.
    ++particleReadSerial_;
    const auto count = [&](M2Instance& inst, const M2ParticlePass::Read& read) {
        if (inst.particleCountedAt != particleReadSerial_) {
            inst.particleCountedAt = particleReadSerial_;
            inst.liveParticles = 0;
            inst.particleReach = 0.0f;
            ++inst.particleCounts;
        }
        inst.liveParticles += read.live;
        inst.particleReach = std::max(inst.particleReach, read.reach);
    };
    for (const M2ParticlePass::Read& read : particlePass_.takeRead()) {
        if (read.owner.posed) {
            auto it = scene_->posedEmitters_.find(read.owner.id);
            if (it != scene_->posedEmitters_.end()) count(it->second.inst, read);
            continue;
        }
        const auto at = scene_->instanceIndexById.find(read.owner.id);
        if (at == scene_->instanceIndexById.end()) continue;  // removed since
        count(scene_->instances[at->second], read);
    }
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
            return m2_track::sampleFloat(track, inst.currentSequenceIndex, animTimeOf(inst),
                                         globalSequenceTimeOf(inst), gpu.globalSequenceDurations, 1.0f);
        };
        const bool visible = sample(em.visibilityTrack) > 0.5f;
        const float above = sample(em.heightAboveTrack);
        const float below = sample(em.heightBelowTrack);
        const float alpha = sample(em.alphaTrack);
        const glm::vec3 color = m2_track::sampleVec3(em.colorTrack, inst.currentSequenceIndex,
            animTimeOf(inst), globalSequenceTimeOf(inst), gpu.globalSequenceDurations, glm::vec3(1.0f));

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
    recordM2Ribbons(cmd, perFrameSet);
}

size_t M2Renderer::gatherM2Ribbons(Frame& frame, const glm::vec3& eye) {
    auto& ribbonDraws_ = frame.ribbonDraws_;
    ribbonDraws_.clear();
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return 0;

    // The edges carry their own vertices, laid across the emitter's Y axis
    // when they were born (updateRibbons). The strip used to be widened here
    // across the line of sight from a spine of single points, which turned
    // every trail flat to the camera: none of them could twist.
    auto& vertices = frame.ribbonVertices;
    size_t written = 0;

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
                vertices.resize((written + 1) * 9);
                float* vd = vertices.data() + written * 9;
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
                draws.push_back({.texSet = texSet, .additive = additive, .firstVertex = firstVert, .vertexCount = vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    };
    // Only the instances that carry emitters, as renderM2Particles does: a
    // ribbon emitter is an emitter as far as particleInstanceIndices_ is
    // concerned (M2ModelGPU::hasEmitters). This walked every instance in the
    // world, asking each whether its model had ribbons - the walk the particle
    // pass dropped for costing 2 ms of a 16 ms frame.
    //
    // Nearest first, so a full buffer cuts the farthest trails short, and
    // then drawn farthest first, over what is behind them.
    emittersInView_.clear();
    const auto addInView = [&](const M2Instance& inst) {
        if (!inst.cachedModel) return;
        const glm::vec3 toEye = glm::vec3(inst.modelMatrix[3]) - eye;
        emittersInView_.push_back({.inst = &inst, .distSq = glm::dot(toEye, toEye)});
    };
    for (size_t idx : scene_->particleInstanceIndices_) {
        if (idx < scene_->instances.size()) addInView(scene_->instances[idx]);
    }
    for (const auto& [id, posed] : scene_->posedEmitters_) addInView(posed.inst);
    sortNearestFirst(emittersInView_);
    emitterBlocks_.clear();
    for (const auto& e : emittersInView_) {
        emitterBlocks_.push_back(draws.size());
        addInstanceRibbons(*e.inst);
    }
    reverseBlocks(draws, emitterBlocks_, reorderedRibbons_);

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (size_t idx : scene_->particleInstanceIndices_) {
                if (idx >= scene_->instances.size()) continue;
                const M2Instance& inst = scene_->instances[idx];
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

    if (draws.empty()) return 0;
    vertices.resize(written * 9);
    return written;
}

void M2Renderer::recordM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    const auto& ribbonDraws_ = recording_->ribbonDraws_;
    if (!frameBuffer(ribbonVB_) || !blended().ribbon || !blended().ribbonAdditive) return;
    if (!recording_->ribbonVertices.empty())
        std::memcpy(frameMapped(ribbonVB_), recording_->ribbonVertices.data(), recording_->ribbonVertices.size() * sizeof(float));
    if (ribbonDraws_.empty()) return;
    const ::VkBuffer ribbonBuffer = frameBuffer(ribbonVB_);
    const auto& draws = ribbonDraws_;

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
        const VkPipeline pipe = dc.additive ? blended().ribbonAdditive : blended().ribbon;
        if (pipe != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = pipe;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonBuffer, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    recordM2Particles(cmd, perFrameSet);
}

size_t M2Renderer::gatherM2Particles(Frame& frame, const glm::vec3& eye, const PosedSkeletons* characters) {
    auto& runs = frame.particleRuns_;
    runs.clear();
    ParticleFrame& work = frame.particles;
    work.clear();
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return 0;
    work.eye = eye;
    work.density = particleDensity_;
    work.blobPages = skeletons_.pageBuffers();
    work.blobVersion = skeletons_.version();
    work.poolPages = particles_.pageBuffers();
    work.poolVersion = particles_.version();
    Frustum viewFrustum;
    viewFrustum.extractFromMatrix(cachedViewProjection_);
    const bool haveLights = frameLights_ && !frameLights_->empty();

    // Each placed emitter of an instance that ran this update, or is seen:
    // a row. One that did not run is not moved (its time is none) - out of
    // view and far, its particles freeze, as in the client - and one unseen
    // lays out no quads. Where it stands: its skeleton as posed this frame,
    // a creature's for a posed set, or its model matrix.
    //
    // Seen ones nearest first and drawn farthest first, as blending needs.
    constexpr float kParticleSizeMargin = 2.0f;
    emittersInView_.clear();
    particleRunScratch_.clear();
    const auto addRows = [&](const M2Instance& inst, bool posedSet, bool emits, std::optional<size_t> index,
                             std::optional<PosedSkeletons::Pose> pose) {
        const M2ModelGPU& model = *inst.cachedModel;
        if (!model.emitters.placed() || inst.particleRanges.empty()) return;
        const bool ran = inst.particlesRanAt == particleSerial_;
        // A posed set has no culling entry: it is drawn wherever it is.
        const glm::vec3 centre = posedSet ? inst.position : inst.cachedCullCenter;
        const float radius = std::max(inst.cachedPaddedRadius, inst.particleReach) + kParticleSizeMargin;
        const bool seen = posedSet || viewFrustum.intersectsSphere(centre, radius);
        if (!ran && !seen) return;
        ParticleRowGPU row;
        row.blobPage = static_cast<uint32_t>(model.emitters.page);
        row.blobFirst = model.emitters.first;
        row.sequence = inst.currentSequenceIndex;
        row.time = animTimeOf(inst);
        row.globalTime = globalSequenceTimeOf(inst);
        row.dt = ran ? particleDt_ : 0.0f;
        row.frameSerial = static_cast<uint32_t>(particleSerial_);
        row.cullCentre = glm::vec4(centre, inst.fade);
        row.placement = inst.modelMatrix;
        row.light = inst.interiorLight;
        row.localLights = haveLights ? pickLocalLights(*frameLights_, glm::vec3(inst.modelMatrix[3]), 0.0f)
                                     : kNoLocalLights;
        uint32_t base = 0;
        if (pose) {
            base = ParticleRowGPU::kCharacterSkeleton;
            row.request = pose->request;
            row.boneBase = pose->boneBase;
            row.boneCount = pose->boneCount;
        } else if (index && inst.cachedHasAnimation && !inst.cachedDisableAnimation && model.skeleton.placed()) {
            base = ParticleRowGPU::kM2Skeleton;
            row.request = poseOf(*index, frame);
            row.boneBase = frame.skeleton.requests[row.request].outBase;
            row.boneCount = model.skeleton.boneCount;
        }
        if (ran && emits) base |= ParticleRowGPU::kEmit;
        if (seen) base |= ParticleRowGPU::kInView;
        if (model.isFireflyEffect) base |= ParticleRowGPU::kFirefly;
        EmitterInView inView{.inst = &inst,
                             .distSq = glm::dot(glm::vec3(inst.modelMatrix[3]) - eye,
                                                glm::vec3(inst.modelMatrix[3]) - eye),
                             .firstRun = static_cast<uint32_t>(particleRunScratch_.size())};
        for (size_t e = 0; e < inst.particleRanges.size() && e < model.particleEmitters.size(); ++e) {
            const ParticlePool::Range& range = inst.particleRanges[e];
            if (!range.placed()) continue;
            const auto& em = model.particleEmitters[e];
            row.emitter = static_cast<uint32_t>(e);
            row.poolPage = static_cast<uint32_t>(range.page);
            row.poolFirst = range.first;
            row.capacity = range.capacity;
            row.tag = range.tag;
            row.seed = emitterSeed(inst.id, static_cast<uint32_t>(e), posedSet);
            // Lit unless flag 0x1, and never Mod or Mod2x (0xa45374).
            row.flags = base;
            if (!(em.flags & pipeline::kM2EmitterUnlit) && !m2BlendIsModulate(em.blendingType))
                row.flags |= ParticleRowGPU::kLit;
            const auto rowIndex = static_cast<uint32_t>(work.rows.size());
            work.rows.push_back(row);
            work.owners.push_back({.id = inst.id, .emitter = static_cast<uint16_t>(e), .posed = posedSet});
            if (!seen || (!em.drawsHead && !em.drawsTail)) continue;
            VkDescriptorSet texSet = e < model.particleTexSets.size() ? model.particleTexSets[e] : VK_NULL_HANDLE;
            if (texSet == VK_NULL_HANDLE) {
                // loadModel could not allocate it: materialDescPool_ is exhausted.
                LOG_FATAL("M2 particles: emitter ", e, " of '", model.name,
                          "' has no descriptor set; loadModel could not allocate one");
                std::abort();
            }
            particleRunScratch_.push_back({.texSet = texSet, .blendType = em.blendingType,
                                           .tilesX = std::max<uint16_t>(em.textureCols, 1),
                                           .tilesY = std::max<uint16_t>(em.textureRows, 1),
                                           .fogged = !(em.flags & pipeline::kM2EmitterUnfogged),
                                           .row = rowIndex});
        }
        inView.runCount = static_cast<uint32_t>(particleRunScratch_.size()) - inView.firstRun;
        if (inView.runCount > 0) emittersInView_.push_back(inView);
    };
    for (size_t idx : scene_->particleInstanceIndices_) {
        if (idx >= scene_->instances.size()) continue;
        const M2Instance& inst = scene_->instances[idx];
        if (!inst.cachedModel || inst.hidden) continue;
        // A finished effect emits nothing more; what it has flies on.
        addRows(inst, false, !inst.finishing, idx, std::nullopt);
    }
    for (const auto& [id, posed] : scene_->posedEmitters_) {
        std::optional<PosedSkeletons::Pose> pose;
        if (characters) pose = characters->poseOf(posed.skeletonOwner);
        addRows(posed.inst, true, posed.posed, std::nullopt, pose);
    }
    sortNearestFirst(emittersInView_);
    for (auto it = emittersInView_.rbegin(); it != emittersInView_.rend(); ++it)
        runs.insert(runs.end(), particleRunScratch_.begin() + it->firstRun,
                    particleRunScratch_.begin() + it->firstRun + it->runCount);
    return work.rows.size();
}

void M2Renderer::recordM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    const auto& runs = recording_->particleRuns_;
    const uint32_t slot = vkCtx_->getCurrentFrame();
    if (runs.empty() || slot >= 2 || !particlesRun_[slot]) return;
    const ::VkBuffer quads = particlePass_.quads(slot);
    const ::VkBuffer draws = particlePass_.draws(slot);
    if (!quads || !draws) return;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipelineLayout_, 0, 1, &perFrameSet, 0,
                            nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quads, &offset);
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    for (const ParticleRun& run : runs) {
        const M2BlendPipeline blendPipe = m2BlendPipeline(run.blendType);
        const VkPipeline pipeline = blended().particle[static_cast<size_t>(blendPipe)];
        if (!pipeline) continue;
        if (pipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            currentPipeline = pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipelineLayout_, 1, 1, &run.texSet, 0,
                                nullptr);
        const M2ParticlePush pc{
            .tileX = static_cast<float>(run.tilesX), .tileY = static_cast<float>(run.tilesY),
            .alphaRef = m2AlphaRef(blendPipe),
            .fogMode = static_cast<int32_t>(run.fogged ? m2FogMode(blendPipe) : M2FogMode::None),
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        // Its quads, as many as the GPU laid out, from where it put them.
        vkCmdDrawIndirect(cmd, draws, VkDeviceSize(run.row) * sizeof(VkDrawIndirectCommand), 1,
                          sizeof(VkDrawIndirectCommand));
    }
}

} // namespace rendering
} // namespace wowee
