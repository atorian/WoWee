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
    bool any = false;
    // A particle emitter's room is its particles' and a header; a ribbon's
    // its edges' and two (m2_ribbons.comp).
    const auto place = [&](std::vector<ParticlePool::Range>& ranges, const std::vector<uint32_t>& capacities,
                           uint32_t headers) {
        if (ranges.size() != capacities.size()) ranges.resize(capacities.size());
        for (size_t e = 0; e < capacities.size(); ++e) {
            if (!ranges[e].placed() && capacities[e] > 0)
                ranges[e] = particles_.place(capacities[e] + headers - 1, model->name.c_str());
            any = any || ranges[e].placed();
        }
    };
    place(inst.particleRanges, model->emitters.capacities, 1);
    place(inst.ribbonRanges, model->emitters.ribbonCapacities, 2);
    return any;
}

void M2Renderer::releaseParticles(M2Instance& inst) {
    for (const ParticlePool::Range& range : inst.particleRanges) particles_.release(range);
    for (const ParticlePool::Range& range : inst.ribbonRanges) particles_.release(range);
    inst.particleRanges.clear();
    inst.ribbonRanges.clear();
    inst.liveParticles = 0;
    inst.liveEdges = 0;
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
            inst.liveEdges = 0;
            inst.particleReach = 0.0f;
            ++inst.particleCounts;
        }
        if (read.owner.ribbon) {
            inst.liveEdges += read.live;
            return;
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

void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    recordM2Ribbons(cmd, perFrameSet);
}

void M2Renderer::recordM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    const auto& runs = recording_->ribbonRuns_;
    const uint32_t slot = vkCtx_->getCurrentFrame();
    if (runs.empty() || slot >= 2 || !particlesRun_[slot] || !blended().ribbon || !blended().ribbonAdditive) return;
    const ::VkBuffer vertices = particlePass_.ribbonVertices(slot);
    const ::VkBuffer draws = particlePass_.draws(slot);
    if (!vertices || !draws) return;

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
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &offset);
    for (const RibbonRun& run : runs) {
        const VkPipeline pipe = run.additive ? blended().ribbonAdditive : blended().ribbon;
        if (pipe != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = pipe;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &run.texSet, 0, nullptr);
        // Its strip, as long as the GPU laid it, from where it put it.
        vkCmdDrawIndirect(cmd, draws, VkDeviceSize(run.row) * sizeof(VkDrawIndirectCommand), 1,
                          sizeof(VkDrawIndirectCommand));
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    recordM2Particles(cmd, perFrameSet);
}

size_t M2Renderer::gatherM2Particles(Frame& frame, const glm::vec3& eye, const PosedSkeletons* characters) {
    auto& runs = frame.particleRuns_;
    auto& ribbonRuns = frame.ribbonRuns_;
    runs.clear();
    ribbonRuns.clear();
    ParticleFrame& work = frame.particles;
    work.clear();
    // Diagnostics: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one, and
    // WOWEE_M2_NO_RIBBONS=1 every ribbon trail.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
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
    // lays out no quads. A ribbon is drawn wherever it is, its strip being
    // anywhere its emitter has been. Where it stands: its skeleton as posed
    // this frame, a creature's for a posed set, or its model matrix.
    //
    // The particle emitters' rows first, then the ribbons' (kept apart here
    // until all are listed); drawn farthest first, as blending needs.
    constexpr float kParticleSizeMargin = 2.0f;
    emittersInView_.clear();
    particleRunScratch_.clear();
    ribbonRunScratch_.clear();
    std::vector<ParticleRowGPU>& ribbonRows = ribbonRowScratch_;
    std::vector<ParticleFrame::Owner>& ribbonOwners = ribbonOwnerScratch_;
    ribbonRows.clear();
    ribbonOwners.clear();
    const auto addRows = [&](const M2Instance& inst, bool posedSet, bool emits, std::optional<size_t> index,
                             std::optional<PosedSkeletons::Pose> pose) {
        const M2ModelGPU& model = *inst.cachedModel;
        const bool particles = !kNoParticles && !inst.particleRanges.empty();
        const bool ribbons = !kNoRibbons && !inst.ribbonRanges.empty();
        if (!model.emitters.placed() || (!particles && !ribbons)) return;
        const bool ran = inst.particlesRanAt == particleSerial_;
        // A posed set has no culling entry: it is drawn wherever it is.
        const glm::vec3 centre = posedSet ? inst.position : inst.cachedCullCenter;
        const float radius = std::max(inst.cachedPaddedRadius, inst.particleReach) + kParticleSizeMargin;
        const bool seen = posedSet || viewFrustum.intersectsSphere(centre, radius);
        if (!ran && !seen && !ribbons) return;
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
        if (model.isFireflyEffect) base |= ParticleRowGPU::kFirefly;
        EmitterInView inView{.inst = &inst,
                             .distSq = glm::dot(glm::vec3(inst.modelMatrix[3]) - eye,
                                                glm::vec3(inst.modelMatrix[3]) - eye),
                             .firstRun = static_cast<uint32_t>(particleRunScratch_.size()),
                             .firstRibbonRun = static_cast<uint32_t>(ribbonRunScratch_.size())};
        // Its ribbons, every one seen.
        for (size_t e = 0; ribbons && e < inst.ribbonRanges.size() && e < model.ribbonEmitters.size(); ++e) {
            const ParticlePool::Range& range = inst.ribbonRanges[e];
            if (!range.placed()) continue;
            row.emitter = static_cast<uint32_t>(e);
            row.poolPage = static_cast<uint32_t>(range.page);
            row.poolFirst = range.first;
            row.capacity = range.capacity - 1;  // its edges; the range has a second header
            row.tag = range.tag;
            row.seed = emitterSeed(inst.id, static_cast<uint32_t>(e), posedSet);
            row.flags = base | ParticleRowGPU::kInView;
            VkDescriptorSet texSet = e < model.ribbonTexSets.size() ? model.ribbonTexSets[e] : VK_NULL_HANDLE;
            if (texSet == VK_NULL_HANDLE) {
                LOG_FATAL("M2 ribbons: ribbon ", e, " of '", model.name,
                          "' has no descriptor set; loadModel could not allocate one");
                std::abort();
            }
            const bool additive = e < model.ribbonBlendModes.size() && model.ribbonBlendModes[e] >= 3;
            ribbonRunScratch_.push_back(
                {.texSet = texSet, .additive = additive, .row = static_cast<uint32_t>(ribbonRows.size())});
            ribbonRows.push_back(row);
            ribbonOwners.push_back(
                {.id = inst.id, .emitter = static_cast<uint16_t>(e), .posed = posedSet, .ribbon = true});
        }
        inView.ribbonRunCount = static_cast<uint32_t>(ribbonRunScratch_.size()) - inView.firstRibbonRun;
        if (seen) base |= ParticleRowGPU::kInView;
        for (size_t e = 0; particles && (ran || seen) && e < inst.particleRanges.size() &&
                           e < model.particleEmitters.size();
             ++e) {
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
        if (inView.runCount > 0 || inView.ribbonRunCount > 0) emittersInView_.push_back(inView);
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
    // The ribbons' rows after the particle emitters'.
    work.ribbonFirst = static_cast<uint32_t>(work.rows.size());
    work.rows.insert(work.rows.end(), ribbonRows.begin(), ribbonRows.end());
    work.owners.insert(work.owners.end(), ribbonOwners.begin(), ribbonOwners.end());
    sortNearestFirst(emittersInView_);
    for (auto it = emittersInView_.rbegin(); it != emittersInView_.rend(); ++it) {
        runs.insert(runs.end(), particleRunScratch_.begin() + it->firstRun,
                    particleRunScratch_.begin() + it->firstRun + it->runCount);
        for (uint32_t r = 0; r < it->ribbonRunCount; ++r) {
            RibbonRun run = ribbonRunScratch_[it->firstRibbonRun + r];
            run.row += work.ribbonFirst;
            ribbonRuns.push_back(run);
        }
    }
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
