#include <atomic>
#include "rendering/placement_transform.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_material.hpp"
#include "rendering/m2_batch_opacity.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_scene.hpp"
#include "core/env_flag.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_blend_mode.hpp"
#include "pipeline/model_bounds.hpp"
#include "rendering/render_constants.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/bone_slots.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <chrono>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <limits>
#include <future>
#include <thread>
#include <set>

#include <string_view>

namespace wowee {
namespace rendering {

namespace {

// A doodad winds its outside counter-clockwise against its own normals, as a
// character does: 172499 of 174844 triangles over 400 stock doodads, and every
// triangle of the Dalaran fountain's crystals. Placement is rotation and a
// positive scale, so nothing on the way to the screen turns that round. Under
// the builder's default of clockwise every outer face reached m2.frag as a
// back face - lit with its normal turned inward, so the sunlit side was the
// dark one, and culled outright on a single-sided blended batch, which left
// the crystals showing their insides.
constexpr VkFrontFace kM2FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

} // namespace

void M2Instance::updateModelMatrix() {
    // Doodads and buildings compose this identically, in placement_transform.hpp
    // - the header records what it took to establish the order, and a test
    // pins it. Composing it here as well is how the two came to disagree.
    modelMatrix = placementModelMatrix(position, rotation, scale);
    invModelMatrix = glm::inverse(modelMatrix);
}

void M2Instance::recomputeCachedCullFactors() {
    // Matrix instances (notably ADT tree doodads) can have an offset pivot and
    // arbitrary scale. A sphere centered at the placement origin with the M2
    // header radius can therefore exclude much of the visible canopy. Derive
    // the render-cull sphere from transformed vertex bounds instead. The
    // separate worldBounds fields intentionally remain collision bounds.
    if (cachedModel) {
        glm::vec3 visualMin(std::numeric_limits<float>::max());
        glm::vec3 visualMax(std::numeric_limits<float>::lowest());
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                for (int z = 0; z < 2; ++z) {
                    const glm::vec3 local(
                        x ? cachedModel->boundMax.x : cachedModel->boundMin.x,
                        y ? cachedModel->boundMax.y : cachedModel->boundMin.y,
                        z ? cachedModel->boundMax.z : cachedModel->boundMin.z);
                    const glm::vec3 world = glm::vec3(modelMatrix * glm::vec4(local, 1.0f));
                    visualMin = glm::min(visualMin, world);
                    visualMax = glm::max(visualMax, world);
                }
            }
        }
        cachedCullCenter = (visualMin + visualMax) * 0.5f;
        cachedVisualRadius = glm::length(visualMax - visualMin) * 0.5f;
    } else {
        cachedCullCenter = position;
        cachedVisualRadius = cachedBoundRadius * scale;
    }

    float worldRadius = cachedVisualRadius;
    float cullRadius = worldRadius;
    if (cachedDisableAnimation) cullRadius = std::max(cullRadius, 3.0f);
    float factor = std::max(1.0f, cullRadius / rendering::M2_CULL_RADIUS_SCALE_DIVISOR);
    if (cachedDisableAnimation) factor *= 2.6f;
    if (cachedIsGroundDetail)   factor *= 0.9f;
    cachedEffectiveMaxDistSqFactor = factor;
    cachedPaddedRadius = std::max(cullRadius * rendering::M2_PADDED_RADIUS_SCALE,
                                  cullRadius + rendering::M2_PADDED_RADIUS_MIN_MARGIN);
}

M2Renderer::M2Renderer() {
}

M2Renderer::~M2Renderer() {
    shutdown();
}

void M2Renderer::gatherLocalLights(const glm::vec3& cameraPos,
                                   std::vector<LocalLight>& out) const {
    // The client's point lights are the M2Lights of the models it animated
    // this frame - those that passed culling, and doodads within ten yards -
    // and nothing else (local_lights.hpp). A lamp out of view lights nothing,
    // even where what it would light is on screen.
    constexpr float kNearDoodadSq = 10.0f * 10.0f;
    for (const size_t index : lightInstanceIndices_) {
        if (index >= instances.size()) continue;
        const auto& inst = instances[index];
        const M2ModelGPU* model = inst.cachedModel;
        if (!model || model->lights.empty() || !inst.cachedIsValid) continue;
        const glm::vec3 toCam = inst.position - cameraPos;
        if (!inst.lastCullVisible && glm::dot(toCam, toCam) > kNearDoodadSq) continue;

        for (const auto& light : model->lights) {
            if (light.type != 1) {
                // A directional M2 light adds to every receiver's ambient and
                // folds into the sun (render spec, M2 lights); not done here.
                static std::unordered_set<std::string> saidDirectional;
                if (saidDirectional.insert(model->name).second) {
                    LOG_WARNING("M2 '", model->name, "' carries a directional light (type ",
                                light.type, "); it is not applied - the model lights nothing with it");
                }
                continue;
            }
            const int seq = inst.currentSequenceIndex;
            const float t = inst.animTime;
            const float gt = inst.globalSequenceTime;
            const auto& gsd = model->globalSequenceDurations;
            // Visibility is a switch: the Dalaran lamp's is 0 until it opens.
            if (m2_track::sampleFloat(light.visibility, seq, t, gt, gsd, 1.0f) < 0.5f) continue;
            // Diffuse alone: a point light's ambient is never read. The
            // model's alpha fades it with the model.
            const glm::vec3 color =
                m2_track::sampleVec3(light.diffuseColor, seq, t, gt, gsd, glm::vec3(1.0f)) *
                m2_track::sampleFloat(light.diffuseIntensity, seq, t, gt, gsd, 1.0f) * inst.fade;
            if (std::max({color.r, color.g, color.b}) <= 0.0f) continue;
            glm::mat4 bone(1.0f);
            if (light.bone >= 0 && static_cast<size_t>(light.bone) < inst.boneMatrices.size()) {
                bone = inst.boneMatrices[static_cast<size_t>(light.bone)];
            }
            out.push_back({.position = glm::vec3(inst.modelMatrix * bone * glm::vec4(light.position, 1.0f)),
                           .color = color});
        }
    }
}

/// The nine main-pass pipelines, built once at startup and again after a
/// device loss.
///
/// Both paths used to build them: initialize() here and recreatePipelines() in
/// m2_renderer_instance.cpp, which was a copy of these 190 lines that had
/// drifted only in its comments. Eighty-seven overlapping twelve-line blocks -
/// the largest duplicate in the tree. A change to any blend state, depth mode
/// or vertex layout landed in whichever copy was in front of whoever made it,
/// and the one that did not get it only showed after a device loss.
///
/// The one thing that genuinely differed is the ribbon pipeline layout, which
/// is created here and is now created only when there is not one already: the
/// rebuild destroys pipelines and keeps layouts.
// The M2 vertex: 18 floats, 72 bytes.
// loc 0: vec3 pos (0), loc 1: vec3 normal (12), loc 2: vec2 uv0 (24),
// loc 5: vec2 uv1 (32), loc 3: vec4 boneWeights (40), loc 4: vec4 boneIndices (56)
static void m2VertexInput(VkVertexInputBindingDescription& binding,
                          std::vector<VkVertexInputAttributeDescription>& attrs) {
    binding = {};
    binding.binding = 0;
    binding.stride = 18 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    attrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},                     // position
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 3 * sizeof(float)},     // normal
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float)},        // texCoord0
        {.location = 5, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 8 * sizeof(float)},        // texCoord1
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 10 * sizeof(float)}, // boneWeights
        {.location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 14 * sizeof(float)}, // boneIndices (float)
    };
}

bool M2Renderer::buildMainPassPipelines(VkDescriptorSetLayout perFrameLayout) {
    VkDevice device = vkCtx_->getDevice();

    // --- Load shaders ---
    rendering::VkShaderModule m2Vert, m2Frag;
    (void)m2Vert.loadFromFile(device, "assets/shaders/m2.vert.spv");
    (void)m2Frag.loadFromFile(device, "assets/shaders/m2.frag.spv");

    if (!m2Vert.isValid() || !m2Frag.isValid()) {
        LOG_ERROR("M2: Missing required shaders, cannot build pipelines");
        return false;
    }

    VkRenderPass mainPass = vkCtx_->getImGuiRenderPass();

    // --- Build M2 model pipelines ---
    VkVertexInputBindingDescription m2Binding{};
    std::vector<VkVertexInputAttributeDescription> m2Attrs;
    m2VertexInput(m2Binding, m2Attrs);

    // Pipeline derivatives - opaque is the base, others derive from it for shared state optimization
    auto buildM2Pipeline = [&](VkPipelineColorBlendAttachmentState blendState, bool depthWrite,
                               VkPipelineCreateFlags flags = 0, VkPipeline basePipeline = VK_NULL_HANDLE,
                               bool alphaToCoverage = false) -> VkPipeline {
        auto builder = PipelineBuilder()
            .setShaders(m2Vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        m2Frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({m2Binding}, m2Attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, kM2FrontFace)
            // The sky model tests depth but never writes it. Its vertices are
            // pushed to the far plane, so the test is what lets ground drawn
            // before it occlude it, and a write would put the far plane over
            // everything drawn after.
            .setDepthTest(true, skyMode_ ? false : depthWrite, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(blendState)
            .setMultisample(vkCtx_->getMsaaSamples());
        // MSAA alpha-to-coverage dithers the shader's sharpened cutout alpha
        // across samples for smooth foliage/leaf silhouettes.
        if (alphaToCoverage) builder.setAlphaToCoverage(true);
        return builder
            .setLayout(pipelineLayout_)
            .setRenderPass(mainPass)
            .setDynamicStates(viewportAndScissorDynamic())
            .setFlags(flags)
            .setBasePipeline(basePipeline)
            .build(device, vkCtx_->getPipelineCache());
    };

    opaquePipeline_ = buildM2Pipeline(PipelineBuilder::blendDisabled(), true,
                                      VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
    alphaTestPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), true,
                                         VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    // Every alpha-tested batch - a canopy, a fern, a tuft of clutter - is drawn
    // through this one. Alpha-to-coverage spreads the shader's sharpened alpha
    // across the samples, so a leaf edge is a coverage ramp rather than a
    // binary in-or-out, and the distance fade has somewhere to land: on the
    // opaque pipeline the cutout path used to bind, both were computed and
    // then thrown away, which is why a canopy read as one hard-edged blob and
    // a doodad popped rather than faded. Blending stays off, so it is still an
    // opaque pass: order-independent, depth written, no halo where a leaf
    // drawn early sits over the sky.
    cutoutPipeline_ = buildM2Pipeline(PipelineBuilder::blendDisabled(), true,
                                      VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_,
                                      /*alphaToCoverage=*/true);
    // Reuse particleTexLayout_ for set 1 (single texture sampler). Only once:
    // a pipeline layout outlives the pipelines built from it, and a
    // device-loss rebuild destroys the pipelines alone. The rebuild path used
    // to be a copy of this function that simply did not have these lines,
    // which is the whole reason the two could drift.
    if (ribbonPipelineLayout_ == VK_NULL_HANDLE) {
        VkDescriptorSetLayout ribLayouts[] = {perFrameLayout, particleTexLayout_};
        VkPipelineLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = 2;
        lci.pSetLayouts = ribLayouts;
        vkCreatePipelineLayout(device, &lci, nullptr, &ribbonPipelineLayout_);
    }

    buildBlendedPipelines(mainPass, vkCtx_->getMsaaSamples(), /*derive=*/true, scenePipes_);

    // Clean up shader modules
    m2Vert.destroy(); m2Frag.destroy();

    return true;
}

bool M2Renderer::buildBlendedPipelines(VkRenderPass pass, VkSampleCountFlagBits samples,
                                       bool derive, BlendedPipelines& out) {
    VkDevice device = vkCtx_->getDevice();
    rendering::VkShaderModule m2Vert, m2Frag;
    rendering::VkShaderModule particleVert, particleFrag;
    rendering::VkShaderModule ribVert, ribFrag;
    (void)m2Vert.loadFromFile(device, "assets/shaders/m2.vert.spv");
    (void)m2Frag.loadFromFile(device, "assets/shaders/m2.frag.spv");
    (void)particleVert.loadFromFile(device, "assets/shaders/m2_particle.vert.spv");
    (void)particleFrag.loadFromFile(device, "assets/shaders/m2_particle.frag.spv");
    (void)ribVert.loadFromFile(device, "assets/shaders/m2_ribbon.vert.spv");
    (void)ribFrag.loadFromFile(device, "assets/shaders/m2_ribbon.frag.spv");
    if (!m2Vert.isValid() || !m2Frag.isValid()) return false;

    // Derived from the opaque pipeline when built for the scene pass it was
    // built for; another pass's pipelines stand alone.
    const VkPipelineCreateFlags flags = derive ? VK_PIPELINE_CREATE_DERIVATIVE_BIT : 0;
    const VkPipeline base = derive ? opaquePipeline_ : VK_NULL_HANDLE;

    VkVertexInputBindingDescription m2Binding{};
    std::vector<VkVertexInputAttributeDescription> m2Attrs;
    m2VertexInput(m2Binding, m2Attrs);
    auto buildM2Pipeline = [&](VkPipelineColorBlendAttachmentState blendState) -> VkPipeline {
        return PipelineBuilder()
            .setShaders(m2Vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        m2Frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({m2Binding}, m2Attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, kM2FrontFace)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(blendState)
            .setMultisample(samples)
            .setLayout(pipelineLayout_)
            .setRenderPass(pass)
            .setDynamicStates(viewportAndScissorDynamic())
            .setFlags(flags)
            .setBasePipeline(base)
            .build(device, vkCtx_->getPipelineCache());
    };
    out.alpha = buildM2Pipeline(PipelineBuilder::blendAlpha());
    out.noAlphaAdd = buildM2Pipeline(m2NoAlphaAddBlendState());
    out.additive = buildM2Pipeline(PipelineBuilder::blendAdditive());
    out.modulate = buildM2Pipeline(m2ModulateBlendState(false));
    out.modulate2x = buildM2Pipeline(m2ModulateBlendState(true));

    // Particles: a quad per instance (an M2ParticleQuad), its four
    // corners made by the vertex shader. They were points, which can only be
    // a square of so many pixels: no tail could be stretched along a flight,
    // and the size had to be guessed from the distance.
    if (particleVert.isValid() && particleFrag.isValid()) {
        VkVertexInputBindingDescription pBind{};
        pBind.binding = 0;
        pBind.stride = sizeof(M2ParticleQuad);
        pBind.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        std::vector<VkVertexInputAttributeDescription> pAttrs = {
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(M2ParticleQuad, position)},
            {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(M2ParticleQuad, color)},
            {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(M2ParticleQuad, halfSize)},
            {.location = 3, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(M2ParticleQuad, cell)},
            {.location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(M2ParticleQuad, axisA)},
            {.location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(M2ParticleQuad, axisB)},
            {.location = 6, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(M2ParticleQuad, spin)},
            {.location = 7, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = offsetof(M2ParticleQuad, shape)},
            {.location = 8, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = offsetof(M2ParticleQuad, lit)},
            {.location = 9, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
             .offset = offsetof(M2ParticleQuad, light) + offsetof(InteriorLightGPU, ambient)},
            {.location = 10, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
             .offset = offsetof(M2ParticleQuad, light) + offsetof(InteriorLightGPU, direct)},
            {.location = 11, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT,
             .offset = offsetof(M2ParticleQuad, localLights)},
        };
        // The client's particle material: two-sided, and depth written only
        // by the unblended modes (Opaque, AlphaKey). Every blend mode has its
        // own state; Add, NoAlphaAdd, Mod and Mod2x used to share two.
        auto buildParticlePipeline = [&](VkPipelineColorBlendAttachmentState blend,
                                         bool depthWrite) -> VkPipeline {
            return PipelineBuilder()
                .setShaders(particleVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                            particleFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                .setVertexInput({pBind}, pAttrs)
                .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .setDepthTest(true, depthWrite, VK_COMPARE_OP_LESS_OR_EQUAL)
                .setColorBlendAttachment(blend)
                .setMultisample(samples)
                .setLayout(particlePipelineLayout_)
                .setRenderPass(pass)
                .setDynamicStates(viewportAndScissorDynamic())
                .build(device, vkCtx_->getPipelineCache());
        };
        auto particlePipe = [&](M2BlendPipeline p) -> VkPipeline& {
            return out.particle[static_cast<size_t>(p)];
        };
        particlePipe(M2BlendPipeline::Opaque) =
            buildParticlePipeline(PipelineBuilder::blendDisabled(), true);
        particlePipe(M2BlendPipeline::AlphaTest) =
            buildParticlePipeline(PipelineBuilder::blendDisabled(), true);
        particlePipe(M2BlendPipeline::Alpha) =
            buildParticlePipeline(PipelineBuilder::blendAlpha(), false);
        particlePipe(M2BlendPipeline::NoAlphaAdd) =
            buildParticlePipeline(m2NoAlphaAddBlendState(), false);
        particlePipe(M2BlendPipeline::AlphaAdd) =
            buildParticlePipeline(PipelineBuilder::blendAdditive(), false);
        particlePipe(M2BlendPipeline::Modulate) =
            buildParticlePipeline(m2ModulateBlendState(false), false);
        particlePipe(M2BlendPipeline::Modulate2x) =
            buildParticlePipeline(m2ModulateBlendState(true), false);
    }

    // Ribbons: pos(3) + color(3) + alpha(1) + uv(2) = 9 floats = 36 bytes
    if (ribVert.isValid() && ribFrag.isValid() && ribbonPipelineLayout_ != VK_NULL_HANDLE) {
        VkVertexInputBindingDescription rBind{};
        rBind.binding = 0;
        rBind.stride = 9 * sizeof(float);
        rBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::vector<VkVertexInputAttributeDescription> rAttrs = {
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},                    // pos
            {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 3 * sizeof(float)},    // color
            {.location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = 6 * sizeof(float)},    // alpha
            {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = 7 * sizeof(float)},    // uv
        };
        auto buildRibbonPipeline = [&](VkPipelineColorBlendAttachmentState blend) -> VkPipeline {
            return PipelineBuilder()
                .setShaders(ribVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                            ribFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                .setVertexInput({rBind}, rAttrs)
                .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
                .setColorBlendAttachment(blend)
                .setMultisample(samples)
                .setLayout(ribbonPipelineLayout_)
                .setRenderPass(pass)
                .setDynamicStates(viewportAndScissorDynamic())
                .build(device, vkCtx_->getPipelineCache());
        };
        out.ribbon = buildRibbonPipeline(PipelineBuilder::blendAlpha());
        out.ribbonAdditive = buildRibbonPipeline(PipelineBuilder::blendAdditive());
    }

    m2Vert.destroy(); m2Frag.destroy();
    particleVert.destroy(); particleFrag.destroy();
    ribVert.destroy(); ribFrag.destroy();
    return out.alpha != VK_NULL_HANDLE;
}

void M2Renderer::destroyBlendedPipelines(BlendedPipelines& pipes) {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    for (VkPipeline* p : {&pipes.alpha, &pipes.noAlphaAdd, &pipes.additive, &pipes.modulate,
                          &pipes.modulate2x, &pipes.ribbon, &pipes.ribbonAdditive}) {
        if (*p) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkPipeline& p : pipes.particle) {
        if (p) vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
}

bool M2Renderer::createFrameVertexBuffer(FrameVertexBuffer& vb, VkDeviceSize size, const char* what) {
    VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.size = size;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        VmaAllocationInfo allocInfo{};
        const VkResult result = vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &vb.buffer[f],
                                                &vb.alloc[f], &allocInfo);
        if (result != VK_SUCCESS || !allocInfo.pMappedData) {
            LOG_ERROR("M2Renderer: could not create the ", what, " vertex buffer (", size,
                      " bytes, frame ", f, ", VkResult ", static_cast<int>(result),
                      ") - those are not drawn");
            destroyFrameVertexBuffer(vb);
            return false;
        }
        vb.mapped[f] = allocInfo.pMappedData;
    }
    return true;
}

void M2Renderer::destroyFrameVertexBuffer(FrameVertexBuffer& vb) {
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        if (vb.buffer[f]) vmaDestroyBuffer(vkCtx_->getAllocator(), vb.buffer[f], vb.alloc[f]);
        vb.buffer[f] = VK_NULL_HANDLE;
        vb.alloc[f] = VK_NULL_HANDLE;
        vb.mapped[f] = nullptr;
    }
}

::VkBuffer M2Renderer::frameBuffer(const FrameVertexBuffer& vb) const {
    return vb.buffer[vkCtx_->getCurrentFrame()];
}

void* M2Renderer::frameMapped(const FrameVertexBuffer& vb) const {
    return vb.mapped[vkCtx_->getCurrentFrame()];
}

void M2Renderer::setAfterWaterPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
    destroyBlendedPipelines(afterWaterPipes_);
    afterWaterPass_ = pass;
    if (!pass || !vkCtx_ || !opaquePipeline_ || skyMode_) return;
    if (!buildBlendedPipelines(pass, samples, /*derive=*/false, afterWaterPipes_)) {
        LOG_WARNING("M2Renderer: could not build pipelines for the water's pass - "
                    "blended doodads stay under the water");
        destroyBlendedPipelines(afterWaterPipes_);
    }
}

bool M2Renderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                            pipeline::AssetManager* assets) {
    if (initialized_) { assetManager = assets; return true; }
    vkCtx_ = ctx;
    assetManager = assets;

    // Announce the renderer diagnostics this build understands, and which of
    // them are active. A run that logs this line is definitely a build that has
    // them, which takes the guesswork out of "did that binary include the fix?".
    LOG_INFO("M2 render diagnostics available (NO_PARTICLES/NO_RIBBONS/NO_SKINNING): ",
             "particles=", core::envFlagEnabled("WOWEE_M2_NO_PARTICLES") ? "OFF" : "on",
             " ribbons=", core::envFlagEnabled("WOWEE_M2_NO_RIBBONS") ? "OFF" : "on",
             " skinning=", core::envFlagEnabled("WOWEE_M2_NO_SKINNING") ? "OFF" : "on",
             " maxBonesPerInstance=", kMaxBonesPerInstance);

    // Instance storage grows to tens of thousands as a session explores, and
    // each doubling reallocates the whole thing mid-frame: measured at 8.9ms
    // crossing 32k and 18.5ms crossing 64k, doubling again each time. Take that
    // allocation up front, where a stall is invisible.
    instances.reserve(65536);

    const unsigned hc = std::thread::hardware_concurrency();
    const size_t availableCores = (hc > 1u) ? static_cast<size_t>(hc - 1u) : 1ull;
    // Keep headroom for other frame tasks: M2 gets about half of non-main cores by default.
    const size_t defaultAnimThreads = std::max<size_t>(1, availableCores / 2);
    numAnimThreads_ = static_cast<uint32_t>(std::max<size_t>(
        1, envSizeOrDefault("WOWEE_M2_ANIM_THREADS", defaultAnimThreads)));
    LOG_INFO("Initializing M2 renderer (Vulkan, ", numAnimThreads_, " anim threads)...");

    VkDevice device = vkCtx_->getDevice();

    // --- Descriptor set layouts ---

    // Material set layout (set 1): binding 0 = sampler2D, binding 2 = M2Material UBO
    // (M2Params moved to push constants alongside model matrix)
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 2;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        bindings[2] = bindings[0];
        bindings[2].binding = 3; // second texture stage (RGBA mask)
        ci.bindingCount = 3;
        ci.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &materialSetLayout_);
    }

    // Bone set layout (set 2): binding 0 = STORAGE_BUFFER (bone matrices)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &boneSetLayout_);
    }

    // Instance data set layout (set 3): binding 0 = STORAGE_BUFFER (per-instance data)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &instanceSetLayout_);
    }

    // Particle texture set layout (set 1 for particles): binding 0 = sampler2D
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &particleTexLayout_);
    }

    // --- Descriptor pools ---
    {
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 * MAX_MATERIAL_SETS + 256},
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_MATERIAL_SETS + 256},
        };
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_MATERIAL_SETS + 256;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &materialDescPool_);
    }
    {
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MAX_BONE_SETS},
        };
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_BONE_SETS;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &boneDescPool_);
    }

    // Create a small identity-bone SSBO + descriptor set so that non-animated
    // draws always have a valid set 2 bound.  The Intel ANV driver segfaults
    // on vkCmdDrawIndexed when a declared descriptor set slot is unbound.
    {
        // Single identity matrix (bone 0 = identity)
        glm::mat4 identity(1.0f);
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizeof(glm::mat4);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocInfo{};
        vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                        &dummyBoneBuffer_, &dummyBoneAlloc_, &allocInfo);
        if (allocInfo.pMappedData) {
            memcpy(allocInfo.pMappedData, &identity, sizeof(identity));
        }

        dummyBoneSet_ = allocateBoneSet();
        if (dummyBoneSet_) {
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = dummyBoneBuffer_;
            bufInfo.offset = 0;
            bufInfo.range = sizeof(glm::mat4);
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = dummyBoneSet_;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // Mega bone SSBO - consolidates all animated instance bones into one buffer per frame.
    // Slot 0 = identity matrix (for non-animated instances), slots 1..N = animated instances.
    {
        const VkDeviceSize megaSize = VkDeviceSize(MEGA_BONE_MATRIX_CAPACITY) * sizeof(glm::mat4);
        glm::mat4 identity(1.0f);
        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = megaSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &megaBoneBuffer_[i], &megaBoneAlloc_[i], &allocInfo);
            megaBoneMapped_[i] = allocInfo.pMappedData;

            // Slot 0: identity matrix (for non-animated instances)
            if (megaBoneMapped_[i]) {
                memcpy(megaBoneMapped_[i], &identity, sizeof(identity));
            }

            megaBoneSet_[i] = allocateBoneSet();
            if (megaBoneSet_[i]) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = megaBoneBuffer_[i];
                bufInfo.offset = 0;
                bufInfo.range = megaSize;
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = megaBoneSet_[i];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }

        // Fresh buffers hold no bone data - invalidate any per-instance upload
        // tracking so prepareRender() re-uploads everything (defensive: instances
        // are normally empty when this runs, but re-init must not skip uploads).
        for (auto& inst : instances) {
            inst.megaBoneUploadedSlot[0] = inst.megaBoneUploadedSlot[1] = 0;
        }
    }

    // Instance data SSBO - per-frame buffer holding per-instance transforms, fade, bones.
    // Shader reads instanceData[push.instanceDataOffset + gl_InstanceIndex].
    {
        static_assert(sizeof(M2InstanceGPU) == 208, "M2InstanceGPU must be 208 bytes (std430)");
        const VkDeviceSize instBufSize = MAX_INSTANCE_DATA * sizeof(M2InstanceGPU);

        // Descriptor pool for 2 sets (double-buffered)
        VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
        VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCi.maxSets = 2;
        poolCi.poolSizeCount = 1;
        poolCi.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(device, &poolCi, nullptr, &instanceDescPool_);

        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = instBufSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &instanceBuffer_[i], &instanceAlloc_[i], &allocInfo);
            instanceMapped_[i] = allocInfo.pMappedData;

            VkDescriptorSetAllocateInfo setAi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setAi.descriptorPool = instanceDescPool_;
            setAi.descriptorSetCount = 1;
            setAi.pSetLayouts = &instanceSetLayout_;
            vkAllocateDescriptorSets(device, &setAi, &instanceSet_[i]);

            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = instanceBuffer_[i];
            bufInfo.offset = 0;
            bufInfo.range = instBufSize;
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = instanceSet_[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // GPU frustum culling - compute pipeline, buffers, descriptors.
    // Compute shader tests each instance bounding sphere against 6 frustum planes + distance.
    // Output: uint visibility[] read back by CPU to skip culled instances in sortedVisible_ build.
    {
        static_assert(sizeof(CullInstanceGPU) == 32, "CullInstanceGPU must be 32 bytes (std430)");
        static_assert(sizeof(CullUniformsGPU) == 272, "CullUniformsGPU must be 272 bytes (std140)");

        // Descriptor set layout: binding 0 = UBO (frustum+camera), 1 = SSBO (input), 2 = SSBO (output)
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutCi.bindingCount = 3;
        layoutCi.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &cullSetLayout_);

        // Pipeline layout (no push constants - everything via UBO)
        VkPipelineLayoutCreateInfo plCi{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &cullSetLayout_;
        vkCreatePipelineLayout(device, &plCi, nullptr, &cullPipelineLayout_);

        // Load compute shader
        rendering::VkShaderModule cullComp;
        if (!cullComp.loadFromFile(device, "assets/shaders/m2_cull.comp.spv")) {
            LOG_ERROR("M2Renderer: failed to load m2_cull.comp.spv - GPU culling disabled");
        } else {
            VkComputePipelineCreateInfo cpCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpCi.stage = cullComp.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
            cpCi.layout = cullPipelineLayout_;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &cullPipeline_) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create cull compute pipeline");
                cullPipeline_ = VK_NULL_HANDLE;
            }
            cullComp.destroy();
        }

        // HiZ-aware cull pipeline (Phase 6.3 Option B)
        // Uses set 0 (same as frustum-only) + set 1 (HiZ pyramid sampler from HiZSystem).
        // The HiZ descriptor set layout is created lazily when hizSystem_ is set, but the
        // pipeline layout and shader are created now if the shader is available.
        rendering::VkShaderModule cullHiZComp;
        if (cullHiZComp.loadFromFile(device, "assets/shaders/m2_cull_hiz.comp.spv")) {
            // HiZ cull set 1 layout: single combined image sampler (the HiZ pyramid)
            VkDescriptorSetLayoutBinding hizBinding{};
            hizBinding.binding = 0;
            hizBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            hizBinding.descriptorCount = 1;
            hizBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayout hizSamplerLayout = VK_NULL_HANDLE;
            VkDescriptorSetLayoutCreateInfo hizLayoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            hizLayoutCi.bindingCount = 1;
            hizLayoutCi.pBindings = &hizBinding;
            vkCreateDescriptorSetLayout(device, &hizLayoutCi, nullptr, &hizSamplerLayout);

            VkDescriptorSetLayout hizSetLayouts[2] = {cullSetLayout_, hizSamplerLayout};
            VkPipelineLayoutCreateInfo hizPlCi{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            hizPlCi.setLayoutCount = 2;
            hizPlCi.pSetLayouts = hizSetLayouts;
            vkCreatePipelineLayout(device, &hizPlCi, nullptr, &cullHiZPipelineLayout_);

            VkComputePipelineCreateInfo hizCpCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            hizCpCi.stage = cullHiZComp.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
            hizCpCi.layout = cullHiZPipelineLayout_;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &hizCpCi, nullptr, &cullHiZPipeline_) != VK_SUCCESS) {
                LOG_WARNING("M2Renderer: failed to create HiZ cull compute pipeline - HiZ disabled");
                cullHiZPipeline_ = VK_NULL_HANDLE;
                vkDestroyPipelineLayout(device, cullHiZPipelineLayout_, nullptr);
                cullHiZPipelineLayout_ = VK_NULL_HANDLE;
            } else {
                LOG_INFO("M2Renderer: HiZ occlusion cull pipeline created");
            }

            // The hizSamplerLayout is now owned by the pipeline layout; we don't track it
            // separately because the pipeline layout keeps a ref. But actually Vulkan
            // requires us to keep it alive. Store it where HiZSystem will provide it.
            // For now, we can destroy it since the pipeline layout was already created.
            vkDestroyDescriptorSetLayout(device, hizSamplerLayout, nullptr);

            cullHiZComp.destroy();
        } else {
            LOG_INFO("M2Renderer: m2_cull_hiz.comp.spv not found - HiZ occlusion culling not available");
        }

        // Descriptor pool: 2 sets × 3 descriptors each (1 UBO + 2 SSBO)
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0] = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2};
        poolSizes[1] = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 4};  // 2 input + 2 output
        VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCi.maxSets = 2;
        poolCi.poolSizeCount = 2;
        poolCi.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolCi, nullptr, &cullDescPool_);

        const VkDeviceSize uniformSize = sizeof(CullUniformsGPU);
        const VkDeviceSize inputSize   = MAX_CULL_INSTANCES * sizeof(CullInstanceGPU);
        const VkDeviceSize outputSize  = MAX_CULL_INSTANCES * sizeof(uint32_t);

        for (int i = 0; i < 2; i++) {
            // Uniform buffer (frustum planes + camera)
            {
                VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = uniformSize;
                bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullUniformBuffer_[i], &cullUniformAlloc_[i], &ai);
                cullUniformMapped_[i] = ai.pMappedData;
            }
            // Input SSBO (per-instance cull data)
            {
                VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = inputSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullInputBuffer_[i], &cullInputAlloc_[i], &ai);
                cullInputMapped_[i] = ai.pMappedData;
            }
            // Output SSBO (visibility flags - GPU writes, CPU reads)
            {
                VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = outputSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullOutputBuffer_[i], &cullOutputAlloc_[i], &ai);
                cullOutputMapped_[i] = ai.pMappedData;
            }

            // Allocate and write descriptor set
            VkDescriptorSetAllocateInfo setAi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setAi.descriptorPool = cullDescPool_;
            setAi.descriptorSetCount = 1;
            setAi.pSetLayouts = &cullSetLayout_;
            vkAllocateDescriptorSets(device, &setAi, &cullSet_[i]);

            VkDescriptorBufferInfo uboInfo{.buffer = cullUniformBuffer_[i], .offset = 0, .range = uniformSize};
            VkDescriptorBufferInfo inputInfo{.buffer = cullInputBuffer_[i], .offset = 0, .range = inputSize};
            VkDescriptorBufferInfo outputInfo{.buffer = cullOutputBuffer_[i], .offset = 0, .range = outputSize};

            VkWriteDescriptorSet writes[3] = {};
            writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[0].dstSet = cullSet_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &uboInfo;

            writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[1].dstSet = cullSet_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &inputInfo;

            writes[2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[2].dstSet = cullSet_[i];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &outputInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    // --- Pipeline layouts ---

    // Main M2 pipeline layout: set 0 = perFrame, set 1 = material, set 2 = bones, set 3 = instances
    // Push constant: M2PushConstants, m2.vert's Push block
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, materialSetLayout_, boneSetLayout_, instanceSetLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = kM2PushConstantBytes;

        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 4;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &pipelineLayout_);
    }

    // Particle pipeline layout: set 0 = perFrame, set 1 = particleTex
    // Push constant: M2ParticlePush (m2_renderer_internal.h)
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, particleTexLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(M2ParticlePush);

        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &particlePipelineLayout_);
    }

    perFrameLayout_ = perFrameLayout;
    if (!buildMainPassPipelines(perFrameLayout)) return false;

    // --- Dynamic particle buffers, mapped for CPU writes, one per frame ---
    // Ribbons: a triangle strip of pos(3)+color(3)+alpha(1)+uv(2).
    createFrameVertexBuffer(m2ParticleVB_, MAX_M2_PARTICLE_QUADS * sizeof(M2ParticleQuad), "particle");
    createFrameVertexBuffer(glowVB_, MAX_GLOW_SPRITES * sizeof(M2ParticleQuad), "glow sprite");
    createFrameVertexBuffer(ribbonVB_, MAX_RIBBON_VERTS * 9 * sizeof(float), "ribbon");

    // --- Create white fallback texture ---
    {
        uint8_t white[] = {255, 255, 255, 255};
        whiteTexture_ = std::make_unique<VkTexture>();
        whiteTexture_->upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
        whiteTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }

    // --- Generate soft radial gradient glow texture ---
    {
        static constexpr int SZ = 64;
        std::vector<uint8_t> px(SZ * SZ * 4);
        float half = SZ / 2.0f;
        for (int y = 0; y < SZ; y++) {
            for (int x = 0; x < SZ; x++) {
                float dx = (x + 0.5f - half) / half;
                float dy = (y + 0.5f - half) / half;
                float r = std::sqrt(dx * dx + dy * dy);
                float a = std::max(0.0f, 1.0f - r);
                a = a * a; // Quadratic falloff
                int idx = (y * SZ + x) * 4;
                px[idx + 0] = 255;
                px[idx + 1] = 255;
                px[idx + 2] = 255;
                px[idx + 3] = static_cast<uint8_t>(a * 255);
            }
        }
        glowTexture_ = std::make_unique<VkTexture>();
        glowTexture_->upload(*vkCtx_, px.data(), SZ, SZ, VK_FORMAT_R8G8B8A8_UNORM);
        glowTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        // Pre-allocate glow texture descriptor set (reused every frame).
        //
        // Valid, not merely built: upload and createSampler both answer nothing
        // here, and a texture missing either writes a null view and sampler
        // into this set for the glow pass to sample. The bind site already
        // skips a null set, so leaving it unallocated loses the glow sprites
        // rather than the device.
        if (glowTexture_->isValid() && particleTexLayout_ && materialDescPool_) {
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(device, &ai, &glowTexDescSet_) == VK_SUCCESS) {
                VkDescriptorImageInfo imgInfo = glowTexture_->descriptorInfo();
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = glowTexDescSet_;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
    }
    textureCacheBudgetBytes_ =
        envSizeMBOrDefault("WOWEE_M2_TEX_CACHE_MB", 4096) * 1024ull * 1024ull;
    modelCacheLimit_ = envSizeMBOrDefault("WOWEE_M2_MODEL_LIMIT", 6000);
    LOG_INFO("M2 texture cache budget: ", textureCacheBudgetBytes_ / (1024 * 1024), " MB");
    LOG_INFO("M2 model cache limit: ", modelCacheLimit_);

    LOG_INFO("M2 renderer initialized (Vulkan)");
    initialized_ = true;
    return true;
}

void M2Renderer::invalidateCullOutput(uint32_t frameIndex) {
    // On non-HOST_COHERENT memory, VMA-mapped GPU→CPU buffers need explicit
    // invalidation so the CPU cache sees the latest GPU writes.
    if (frameIndex < 2 && cullOutputAlloc_[frameIndex]) {
        vmaInvalidateAllocation(vkCtx_->getAllocator(), cullOutputAlloc_[frameIndex], 0, VK_WHOLE_SIZE);
    }
}

void M2Renderer::shutdown() {
    LOG_INFO("Shutting down M2 renderer...");
    if (!vkCtx_) return;

    vkDeviceWaitIdle(vkCtx_->getDevice());
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator alloc = vkCtx_->getAllocator();

    // Delete model GPU resources
    for (auto& [id, model] : models) {
        destroyModelGPU(model);
    }
    models.clear();
    pinnedModelIds_.clear();
    destroyPosedEmitterModels();

    // Destroy instance bone buffers
    for (auto& inst : instances) {
        destroyInstanceBones(inst);
    }
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    cullGrid_.clear();
    instanceDedupMap_.clear();
    // Model and bone destruction above is deferred; drain it now while the
    // descriptor pools are still alive.
    vkCtx_->flushDeferredCleanup();

    // Delete cached textures. ~VkTexture is empty by design -- it has no device
    // or allocator to free with -- so clearing the map on its own drops the
    // unique_ptrs and leaks every image, view and allocation behind them.
    for (auto& [path, entry] : textureCache) {
        if (entry.texture) entry.texture->destroy(device, alloc);
    }
    textureCache.clear();
    // The singletons the cache never held. Same reason as above: a
    // unique_ptr<VkTexture> releases nothing on its own.
    if (whiteTexture_) { whiteTexture_->destroy(device, alloc); whiteTexture_.reset(); }
    if (glowTexture_)  { glowTexture_->destroy(device, alloc);  glowTexture_.reset(); }
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    texturePropsByPtr_.clear();
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;
    whiteTexture_.reset();
    glowTexture_.reset();

    // Clean up particle/ribbon buffers
    destroyFrameVertexBuffer(m2ParticleVB_);
    destroyFrameVertexBuffer(glowVB_);
    destroyFrameVertexBuffer(ribbonVB_);

    // Destroy pipelines
    auto destroyPipeline = [&](VkPipeline& p) { if (p) { vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; } };
    destroyPipeline(opaquePipeline_);
    destroyPipeline(cutoutPipeline_);
    destroyPipeline(alphaTestPipeline_);
    destroyBlendedPipelines(scenePipes_);
    destroyBlendedPipelines(afterWaterPipes_);

    destroy(device, pipelineLayout_);
    destroy(device, particlePipelineLayout_);
    destroy(device, ribbonPipelineLayout_);

    // Destroy descriptor pools and layouts
    destroy(alloc, dummyBoneBuffer_, dummyBoneAlloc_);
    // dummyBoneSet_ is freed implicitly when boneDescPool_ is destroyed
    dummyBoneSet_ = VK_NULL_HANDLE;
    // Mega bone SSBO cleanup (sets freed implicitly with boneDescPool_)
    for (int i = 0; i < 2; i++) {
        destroy(alloc, megaBoneBuffer_[i], megaBoneAlloc_[i]);
        megaBoneMapped_[i] = nullptr;
        megaBoneSet_[i] = VK_NULL_HANDLE;
    }
    destroy(device, materialDescPool_);
    if (boneDescPool_) {
        if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
        vkDestroyDescriptorPool(device, boneDescPool_, nullptr);
        boneDescPool_ = VK_NULL_HANDLE;
    }
    // Instance data SSBO cleanup (sets freed with instanceDescPool_)
    for (int i = 0; i < 2; i++) {
        destroy(alloc, instanceBuffer_[i], instanceAlloc_[i]);
        instanceMapped_[i] = nullptr;
        instanceSet_[i] = VK_NULL_HANDLE;
    }
    destroy(device, instanceDescPool_);

    // GPU frustum culling compute pipeline + buffers cleanup
    destroy(device, cullHiZPipeline_);
    destroy(device, cullHiZPipelineLayout_);
    destroy(device, cullPipeline_);
    destroy(device, cullPipelineLayout_);
    for (int i = 0; i < 2; i++) {
        destroy(alloc, cullUniformBuffer_[i], cullUniformAlloc_[i]);
        destroy(alloc, cullInputBuffer_[i], cullInputAlloc_[i]);
        destroy(alloc, cullOutputBuffer_[i], cullOutputAlloc_[i]);
        cullUniformMapped_[i] = cullInputMapped_[i] = cullOutputMapped_[i] = nullptr;
        cullSet_[i] = VK_NULL_HANDLE;
    }
    destroy(device, cullDescPool_);
    destroy(device, cullSetLayout_);

    destroy(device, materialSetLayout_);
    destroy(device, boneSetLayout_);
    destroy(device, instanceSetLayout_);
    destroy(device, particleTexLayout_);

    // Destroy shadow resources
    destroyPipeline(shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    for (auto& pool : shadowTexPool_) { if (pool) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; } }
    destroyShadowParamsSet(device, alloc, shadowParams_);

    initialized_ = false;
}

void M2Renderer::destroyModelGPU(M2ModelGPU& model) {
    if (!vkCtx_) return;
    releaseRtModel(model);
    VmaAllocator alloc = vkCtx_->getAllocator();
    destroy(alloc, model.vertexBuffer, model.vertexAlloc);
    destroy(alloc, model.indexBuffer, model.indexAlloc);
    VkDevice device = vkCtx_->getDevice();
    for (auto& batch : model.batches) {
        if (batch.materialSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &batch.materialSet); batch.materialSet = VK_NULL_HANDLE; }
        destroy(alloc, batch.materialUBO, batch.materialUBOAlloc);
    }
    // Free pre-allocated particle texture descriptor sets
    for (auto& pSet : model.particleTexSets) {
        if (pSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &pSet); pSet = VK_NULL_HANDLE; }
    }
    model.particleTexSets.clear();
    // Free ribbon texture descriptor sets
    for (auto& rSet : model.ribbonTexSets) {
        if (rSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &rSet); rSet = VK_NULL_HANDLE; }
    }
    model.ribbonTexSets.clear();
}

void M2Renderer::destroyInstanceBones(M2Instance& inst, bool defer) {
    if (!vkCtx_) return;
    releaseInstanceBones(*vkCtx_, boneDescPool_, boneDescPoolGeneration_, inst, defer);
}

VkDescriptorSet M2Renderer::allocateMaterialSet() {
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = materialDescPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &materialSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set);
    if (result != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: material descriptor set allocation failed (", result, ")");
        return VK_NULL_HANDLE;
    }
    return set;
}

VkDescriptorSet M2Renderer::allocateBoneSet() {
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = boneDescPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &boneSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set);
    if (result != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: bone descriptor set allocation failed (", result, ")");
        return VK_NULL_HANDLE;
    }
    return set;
}

// ---------------------------------------------------------------------------
// M2 collision mesh: build spatial grid + classify triangles
// ---------------------------------------------------------------------------
void M2ModelGPU::CollisionMesh::build() {
    if (indices.size() < 3 || vertices.empty()) return;
    triCount = static_cast<uint32_t>(indices.size() / 3);

    // Bounding box for grid
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(-std::numeric_limits<float>::max());
    for (const auto& v : vertices) {
        bmin = glm::min(bmin, v);
        bmax = glm::max(bmax, v);
    }
    boundMin = bmin;
    boundMax = bmax;

    gridOrigin = glm::vec2(bmin.x, bmin.y);
    gridCellsX = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.x - bmin.x) / CELL_SIZE))));
    gridCellsY = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.y - bmin.y) / CELL_SIZE))));

    cellFloorTris.resize(static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY));
    cellWallTris.resize(static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY));
    triBounds.resize(triCount);

    for (uint32_t ti = 0; ti < triCount; ti++) {
        uint16_t i0 = indices[ti * 3];
        uint16_t i1 = indices[ti * 3 + 1];
        uint16_t i2 = indices[ti * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

        const auto& v0 = vertices[i0];
        const auto& v1 = vertices[i1];
        const auto& v2 = vertices[i2];

        triBounds[ti].minZ = std::min({v0.z, v1.z, v2.z});
        triBounds[ti].maxZ = std::max({v0.z, v1.z, v2.z});

        glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
        float normalLen = glm::length(normal);
        float absNz = (normalLen > 0.001f) ? std::abs(normal.z / normalLen) : 0.0f;
        bool isFloor = (absNz >= 0.35f);  // ~70° max slope (relaxed for steep stairs)
        bool isWall  = (absNz < 0.65f);

        float triMinX = std::min({v0.x, v1.x, v2.x});
        float triMaxX = std::max({v0.x, v1.x, v2.x});
        float triMinY = std::min({v0.y, v1.y, v2.y});
        float triMaxY = std::max({v0.y, v1.y, v2.y});

        int cxMin = std::clamp(static_cast<int>((triMinX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cxMax = std::clamp(static_cast<int>((triMaxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cyMin = std::clamp(static_cast<int>((triMinY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
        int cyMax = std::clamp(static_cast<int>((triMaxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);

        for (int cy = cyMin; cy <= cyMax; cy++) {
            for (int cx = cxMin; cx <= cxMax; cx++) {
                int ci = cy * gridCellsX + cx;
                if (isFloor) cellFloorTris[ci].push_back(ti);
                if (isWall)  cellWallTris[ci].push_back(ti);
            }
        }
    }
}

/// The triangles of one cell array that a query box reaches, deduplicated.
///
/// Floors and walls are asked separately and the two queries differed in one
/// token: which array they read. A triangle spanning several cells is filed
/// under each, so the sort and unique are not tidiness - a caller that tests
/// the same triangle twice counts two hits, and a raycast then reports an even
/// number of crossings where there was one surface.
void M2ModelGPU::CollisionMesh::gatherTrisInRange(
        const std::vector<std::vector<uint32_t>>& cells,
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0) return;

    const int cxMin = std::clamp(static_cast<int>((minX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    const int cxMax = std::clamp(static_cast<int>((maxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    const int cyMin = std::clamp(static_cast<int>((minY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
    const int cyMax = std::clamp(static_cast<int>((maxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);

    const size_t cellCount = static_cast<size_t>(cxMax - cxMin + 1) *
                             static_cast<size_t>(cyMax - cyMin + 1);
    out.reserve(cellCount * 8);
    for (int cy = cyMin; cy <= cyMax; cy++) {
        for (int cx = cxMin; cx <= cxMax; cx++) {
            const auto& cell = cells[cy * gridCellsX + cx];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void M2ModelGPU::CollisionMesh::getFloorTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellFloorTris, minX, minY, maxX, maxY, out);
}

void M2ModelGPU::CollisionMesh::getWallTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellWallTris, minX, minY, maxX, maxY, out);
}

bool M2Renderer::hasModel(uint32_t modelId) const {
    return models.find(modelId) != models.end();
}

VkDescriptorImageInfo M2Renderer::batchImageInfo(const VkTexture& texture,
                                                 uint8_t texFlags) const {
    VkDescriptorImageInfo info = texture.descriptorInfo();
    const VkSampler sampler = vkCtx_->getOrCreateSampler(m2TextureSamplerInfo(
        texFlags, texture.getMipLevels(), skyMode_ ? m2SkyMipBias() : 0.0f));
    if (sampler != VK_NULL_HANDLE) info.sampler = sampler;
    return info;
}

void M2Renderer::applyBatchTextureProps(M2ModelGPU::BatchGPU& batch) const {
    // What the texture is, measured from the image. Nothing about how a batch
    // is drawn comes from its texture's file name: the client never keys out
    // black. A forge's or a bonfire's batches with an opaque material are its
    // coals, logs, rocks and lava surfaces - ELWYNNCAMPFIREROCK02,
    // DALARAN_FORGECOALS, LAVA.30 - and their flames are particle emitters
    // (OrcBonFire draws FlameLickSmall and LavaLump2 from its two emitters,
    // not from a batch). Keying "fire" textures cut holes in the rocks.
    auto pit = texturePropsByPtr_.find(batch.texture);
    if (pit == texturePropsByPtr_.end()) return;
    batch.hasAlpha = pit->second.hasAlpha;
    batch.alphaIsSilhouette = pit->second.alphaIsSilhouette;
}

void M2Renderer::writeBatchDescriptors(M2ModelGPU::BatchGPU& batch) const {
    if (!batch.materialSet) return;
    VkTexture* batchTex = (batch.texture && batch.texture->isValid())
        ? batch.texture : whiteTexture_.get();
    VkDescriptorImageInfo imgInfo = batchImageInfo(*batchTex, batch.texFlags);

    VkDescriptorBufferInfo matBufInfo{};
    matBufInfo.buffer = batch.materialUBO;
    matBufInfo.offset = 0;
    matBufInfo.range = sizeof(M2MaterialUBO);

    VkWriteDescriptorSet writes[3] = {};
    // binding 0: texture
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = batch.materialSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imgInfo;
    // binding 2: M2Material UBO
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = batch.materialSet;
    writes[1].dstBinding = 2;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &matBufInfo;

    const bool hasMask = batch.secondTexture && batch.secondTexture->isValid();
    VkDescriptorImageInfo maskInfo = hasMask
        ? batchImageInfo(*batch.secondTexture, batch.secondTexFlags) : imgInfo;
    writes[2] = writes[0];
    writes[2].dstBinding = 3;
    writes[2].pImageInfo = &maskInfo;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 3, writes, 0, nullptr);
}

void M2Renderer::retryPendingBatchTextures() {
    // loadTexture itself decides when a failed key is worth reading again;
    // asking before then is a map lookup and returns the white fallback.
    for (size_t i = 0; i < pendingBatchTextures_.size();) {
        const PendingBatchTexture pending = pendingBatchTextures_[i];
        auto it = models.find(pending.modelId);
        M2ModelGPU::BatchGPU* batch =
            (it != models.end() && pending.batchIndex < it->second.batches.size())
                ? &it->second.batches[pending.batchIndex] : nullptr;
        if (!batch || batch->pendingTextureKey.empty()) {
            pendingBatchTextures_[i] = pendingBatchTextures_.back();
            pendingBatchTextures_.pop_back();
            continue;
        }
        VkTexture* tex = loadTexture(batch->pendingTextureKey, batch->pendingTextureFlags);
        if (tex == nullptr || tex == whiteTexture_.get()) {
            ++i;
            continue;
        }
        // The batch has been hidden since it loaded, so nothing in flight
        // refers to its descriptor set or material buffer.
        const M2ModelGPU& model = it->second;
        batch->texture = tex;
        applyBatchTextureProps(*batch);
        writeBatchMaterial(model, *batch);
        writeBatchDescriptors(*batch);
        batch->batchOpacity = batch->loadedOpacity;
        LOG_INFO("M2 batch shown after retry: '", model.name, "' batch ", pending.batchIndex,
                 " got '", batch->pendingTextureKey, "'");
        batch->pendingTextureKey.clear();
        pendingBatchTextures_[i] = pendingBatchTextures_.back();
        pendingBatchTextures_.pop_back();
    }
}

void M2Renderer::writeBatchMaterial(const M2ModelGPU& model,
                                    M2ModelGPU::BatchGPU& batch) const {
    M2BatchMaterialInputs in;
    in.blendMode = static_cast<uint8_t>(batch.blendMode);
    in.hasAlpha = batch.hasAlpha;
    in.alphaIsSilhouette = batch.alphaIsSilhouette;
    in.spellEffect = model.isSpellEffect;
    in.groundDetail = model.isGroundDetail;
    in.foliageLike = model.isFoliageLike;
    batch.drawState = m2BatchDrawState(in);
    if (!batch.materialUBOMapped) return;

    M2MaterialUBO mat{};
    mat.hasTexture = (batch.texture != nullptr && batch.texture != whiteTexture_.get()) ? 1 : 0;
    mat.alphaTest = batch.drawState.alphaTest;
    mat.tintR = batch.tint.r;
    mat.tintG = batch.tint.g;
    mat.tintB = batch.tint.b;
    mat.unlit = (batch.materialFlags & 0x01) && !batch.drawState.litOverride ? 1 : 0;
    mat.blendMode = batch.blendMode;
    mat.combiner = static_cast<int32_t>(batch.combiner);
    mat.hasSecondTexture = batch.secondTexture && batch.secondTexture->isValid() ? 1 : 0;
    mat.cullBackFaces = !skyMode_ &&
        batch.blendMode >= 2 && (batch.materialFlags & 0x04) == 0 ? 1 : 0;
    mat.specularIntensity = 0.5f;
    mat.emissiveBoost = batch.preserveGlowMesh ? 2.4f : 1.0f;
    std::memcpy(batch.materialUBOMapped, &mat, sizeof(mat));
}

void M2Renderer::markModelAsSpellEffect(uint32_t modelId) {
    auto it = models.find(modelId);
    if (it != models.end()) {
        it->second.isSpellEffect = true;
        for (auto& batch : it->second.batches) writeBatchMaterial(it->second, batch);
        // Spell effects MUST have bone animation for ribbons/particles to work.
        // The classifier may have set disableAnimation=true based on name tokens
        // (e.g. "chest" in HolySmite_Low_Chest.m2) - override that for spell effects.
        if (it->second.disableAnimation && it->second.hasAnimation) {
            it->second.disableAnimation = false;
            LOG_INFO("SpellEffect: re-enabled animation for '", it->second.name, "'");
        }
    }
}

void M2Renderer::censusInstance(const M2Instance& instance) {
    static const bool kCensus = std::getenv("WOWEE_M2_CENSUS") != nullptr;
    if (!kCensus) return;
    auto it = models.find(instance.modelId);
    if (it == models.end()) return;
    const M2ModelGPU& gpu = it->second;
    static std::set<std::string> said;
    if (!said.insert(gpu.name).second) return;
    const float authored = gpu.boundMax.z - gpu.boundMin.z;
    LOG_WARNING("M2 census instance: '", gpu.name, "' scale=", instance.scale,
                " authoredH=", authored, " drawnH=", authored * instance.scale,
                " top=", instance.position.z + gpu.boundMax.z * instance.scale);
}

bool M2Renderer::loadModel(const pipeline::M2Model& model, uint32_t modelId) {
    if (models.find(modelId) != models.end()) {
        // Already loaded
        return true;
    }
    if (models.size() >= modelCacheLimit_) {
        if (modelLimitRejectWarnings_ < 3) {
            LOG_WARNING("M2 model cache full (", models.size(), "/", modelCacheLimit_,
                        "), skipping model load: id=", modelId, " name=", model.name);
        }
        ++modelLimitRejectWarnings_;
        return false;
    }

    // Every model this renderer takes on, named once.
    //
    // Creatures say what they draw through the spawner, and doodads are placed
    // from ADTs that can be read offline - but a spell visual, an attached
    // effect or anything else spawned at runtime appears in no list at all.
    // The Elemental Slave's white sheets are the case: its own model, skins,
    // particles, ribbons, bones and vertex weights were each measured and
    // found correct, which leaves something drawn beside it that nothing names.
    //
    // Cheap enough to leave on - once per model, and a session loads a few
    // hundred - and it is the list every "what is that thing" question starts
    // from.
    // Off unless asked for. This is an inventory, not a fault: it names every
    // model the renderer takes on, which is 197 lines of a 884-line log and
    // the largest single source in it. Worth having - it is what finally
    // showed that an imported override, not the shipped model, was what the
    // client drew - and not worth carrying every session.
    //
    // Two budgets, because one is eaten by the other. A zone's terrain streams
    // in hundreds of doodads within a second or two of arriving, and on the
    // first run of this the four hundred were spent before the creature that
    // prompted it had even spawned. Doodads are placed from ADTs and can be
    // enumerated offline; what cannot is anything spawned at runtime, so that
    // gets the larger share and its own allowance.
    static const bool kLoadDiag = core::envFlagEnabled("WOWEE_M2_LOAD_DIAG", false);
    if (kLoadDiag) {
        const bool placedDoodad = model.name.rfind("WORLD\\", 0) == 0 ||
                                  model.name.rfind("world\\", 0) == 0;
        static core::LogBudget doodadLoadBudget(120, "placed doodad models named at load");
        static core::LogBudget spawnedLoadBudget(600, "spawned models named at load");
        core::LogBudget& budget = placedDoodad ? doodadLoadBudget : spawnedLoadBudget;
        if (budget.take()) {
            LOG_WARNING("M2 load: '", model.name.empty() ? "<unnamed>" : model.name,
                        "' id=", modelId,
                        " verts=", model.vertices.size(),
                        " emitters=", model.particleEmitters.size(),
                        " ribbons=", model.ribbonEmitters.size());
        }
    }

    bool hasGeometry = !model.vertices.empty() && !model.indices.empty();
    bool hasParticles = !model.particleEmitters.empty();
    bool hasRibbons   = !model.ribbonEmitters.empty();
    if (!hasGeometry && !hasParticles && !hasRibbons) {
        LOG_WARNING("M2 model has no renderable content: id=", modelId,
                    " name=", model.name.empty() ? "<unnamed>" : model.name);
        return false;
    }

    M2ModelGPU gpuModel;
    gpuModel.name = model.name;

    // Use tight bounds from actual vertices for collision/camera occlusion.
    // Header bounds in some M2s are overly conservative.
    glm::vec3 tightMin(0.0f);
    glm::vec3 tightMax(0.0f);
    if (hasGeometry) {
        tightMin = glm::vec3(std::numeric_limits<float>::max());
        tightMax = glm::vec3(-std::numeric_limits<float>::max());
        for (const auto& v : model.vertices) {
            // Skip NaN-positioned vertices - would corrupt the bounds
            // (glm::min on NaN is implementation-defined) and feed NaN
            // into the camera-occlusion / culling AABB.
            if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) ||
                !std::isfinite(v.position.z)) continue;
            tightMin = glm::min(tightMin, v.position);
            tightMax = glm::max(tightMax, v.position);
        }
        // If all vertices were NaN (very unlikely after the loader scrub
        // but defense in depth), fall back to a unit box around origin.
        if (tightMin.x > tightMax.x) {
            tightMin = glm::vec3(-1.0f);
            tightMax = glm::vec3(1.0f);
        }
    }

    // Classify model from name and geometry - pure function, no GPU dependencies.
    auto cls = classifyM2Model(model.name, tightMin, tightMax,
                                model.vertices.size(),
                                model.particleEmitters.size());
    const bool isInvisibleTrap   = cls.isInvisibleTrap;
    const bool groundDetailModel = cls.isGroundDetail;
    if (isInvisibleTrap) {
        LOG_INFO("Loading InvisibleTrap model: ", model.name, " (will be invisible, no collision)");
    }

    gpuModel.isInvisibleTrap             = cls.isInvisibleTrap;
    gpuModel.isGroundDetail              = cls.isGroundDetail;
    gpuModel.isFoliageLike               = cls.isFoliageLike;
    gpuModel.disableAnimation            = cls.disableAnimation;
    gpuModel.shadowWindFoliage           = cls.shadowWindFoliage;
    // A banner whose own bones move its cloth does not want the procedural
    // sway on top of it. forsakenbanner01 at the Undercity gate is the shape:
    // 51 of its 85 vertices sit on a root bone with no keys at all - the pole
    // - and the other 34 hang off a four-bone chain the artist animated. The
    // sway is weighted by how far a vertex is below the model's top, which on
    // a planted standard throws the foot of the pole furthest and the cloth
    // least. That is the pole swinging like the banner.
    //
    // A tapestry with no animation of its own still gets the sway; it is the
    // only motion it will ever have.
    // A banner with no cloth anywhere in its texture list is a post.
    //
    // flagpole01, which carries the blue standards at the Undercity gate, is
    // one bone, no animation, and a single texture called POLE1 - ten yards of
    // timber that the name test claimed because "flagpole" contains "flag".
    // Nothing about its geometry says otherwise either: it is uniform top to
    // bottom, so the foot-width test below reads it as a hanging sheet.
    //
    // What does say so is what it is painted with. Over every cloth-named
    // model the client ships, four have a texture list that names only
    // structure and no cloth at all - this pole, a signpost, a wooden banner
    // stand and a stone banner post - and every genuine banner names its own
    // cloth. There are no near misses to trade off.
    bool allStructure = false;
    if (cls.isHangingCloth) {
        static constexpr std::string_view kStructure[] = {
            "pole", "post", "wood", "timber", "plank", "iron", "metal",
            "stone", "rim", "chain", "rope",
        };
        static constexpr std::string_view kCloth[] = {
            "banner", "flag", "tapestry", "pennant", "cloth", "silk",
            "fabric", "drape",
        };
        bool anyNamed = false, everyOneStructure = true, anyCloth = false;
        for (const auto& tex : model.textures) {
            if (tex.filename.empty()) continue;
            anyNamed = true;
            std::string file = tex.filename;
            if (const size_t slash = file.find_last_of("\\/"); slash != std::string::npos) {
                file = file.substr(slash + 1);
            }
            for (char& c : file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto names = [&file](const auto& list) {
                for (std::string_view token : list) {
                    if (file.find(token) != std::string_view::npos) return true;
                }
                return false;
            };
            if (!names(kStructure)) everyOneStructure = false;
            if (names(kCloth)) anyCloth = true;
        }
        allStructure = anyNamed && everyOneStructure && !anyCloth;
        if (allStructure) {
            LOG_INFO("Not cloth: '", model.name,
                     "' is named for a banner but painted only as structure");
        }
    }

    bool clothMovesItself = false;
    if (cls.isHangingCloth && !allStructure) {
        const auto keyed = [](const pipeline::M2AnimationTrack& track) {
            for (const auto& seq : track.sequences) {
                if (seq.timestamps.size() > 1) return true;
            }
            return false;
        };
        for (const auto& bone : model.bones) {
            if (keyed(bone.rotation) || keyed(bone.translation)) {
                clothMovesItself = true;
                break;
            }
        }
    }
    gpuModel.isHangingCloth              =
        cls.isHangingCloth && !clothMovesItself && !allStructure;

    // Held at the foot, not the head.
    //
    // A tapestry nailed to a bar and a standard on a planted pole both reach
    // z=0 and neither bound says which is which, so the geometry does: down at
    // the ground a standard has only the cross-section of its pole, while a
    // hanging cloth still has its full hem. Measured over the shipped models
    // the two do not overlap - forsakenbanner01 is 0.10 of its width down
    // there and diremaul_banner_post 0.04, against 0.35 and up for everything
    // that hangs - so a quarter separates them with room on both sides.
    if (gpuModel.isHangingCloth) {
        const float span = tightMax.z - tightMin.z;
        if (span > 0.01f) {
            const float footTop = tightMin.z + span * 0.15f;
            float footW = 0.0f;
            bool anyFoot = false;
            glm::vec2 footMin(std::numeric_limits<float>::max());
            glm::vec2 footMax(-std::numeric_limits<float>::max());
            for (const auto& v : model.vertices) {
                if (!std::isfinite(v.position.z) || v.position.z > footTop) continue;
                if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y)) continue;
                footMin = glm::min(footMin, glm::vec2(v.position));
                footMax = glm::max(footMax, glm::vec2(v.position));
                anyFoot = true;
            }
            const float fullW = std::max(tightMax.x - tightMin.x, tightMax.y - tightMin.y);
            if (anyFoot && fullW > 0.01f) {
                footW = std::max(footMax.x - footMin.x, footMax.y - footMin.y);
                gpuModel.isStandingCloth = (footW / fullW) < 0.25f;
            }
        }
    }
    gpuModel.isFireflyEffect             = cls.isFireflyEffect;
    gpuModel.isSmallFoliage              = cls.isSmallFoliage;
    gpuModel.isSpellEffect               = cls.isSpellEffect;
    gpuModel.isLavaModel                 = cls.isLavaModel;
    gpuModel.isWaterVegetation           = cls.isWaterVegetation;
    gpuModel.isElvenLike                 = cls.isElvenLike;
    gpuModel.isLanternLike               = cls.isLanternLike;
    gpuModel.isWaterfall                 = cls.isWaterfall;
    gpuModel.isBrazierOrFire             = cls.isBrazierOrFire;
    gpuModel.isGroundFire                = cls.isGroundFire;
    gpuModel.isForge                     = cls.isForge;
    gpuModel.isTorch                     = cls.isTorch;
    // Data-driven flight-path detection: name tokens miss many flying doodads
    // (buzzards, swallows, bird swarms, ...). A small mesh whose bone animation
    // translates it tens of units is a flight-path doodad - it visibly freezes
    // mid-air whenever distance culling stops its bone updates, so give it the
    // same treatment as named sky birds.
    bool flightPathDoodad = cls.isSkyBird;
    if (!flightPathDoodad && !cls.disableAnimation) {
        glm::vec3 meshExtent = tightMax - tightMin;
        const bool smallMesh = meshExtent.x < 6.0f && meshExtent.y < 6.0f &&
                               meshExtent.z < 6.0f;
        if (smallMesh) {
            constexpr float kFlightPathRange = 15.0f;
            for (const auto& bone : model.bones) {
                for (const auto& seq : bone.translation.sequences) {
                    for (const auto& v : seq.vec3Values) {
                        if (std::abs(v.x) > kFlightPathRange ||
                            std::abs(v.y) > kFlightPathRange ||
                            std::abs(v.z) > kFlightPathRange) {
                            flightPathDoodad = true;
                            break;
                        }
                    }
                    if (flightPathDoodad) break;
                }
                if (flightPathDoodad) break;
            }
            if (flightPathDoodad) {
                LOG_DEBUG("Flight-path doodad detected (unnamed sky bird): ", model.name);
            }
        }
    }
    gpuModel.isSkyBird                   = flightPathDoodad;
    gpuModel.isLightBeam                 = cls.isLightBeam;
    // WOWEE_M2_CENSUS=1: every model that loads, once, with what it is made
    // of and what the classifier made of it.
    //
    // Gated on the fire classification, this said nothing at all - and that
    // was the answer: hasWord(n, "bonfire") wants "bonfire" delimited, so
    // ORCPVPBONFIRELARGE is not a fire as far as the classifier is concerned,
    // and neither is anything else whose name runs its words together. A
    // screenshot cannot be grepped and guessing at the model has cost two
    // rounds, so this just lists them.
    static const bool kCensus = std::getenv("WOWEE_M2_CENSUS") != nullptr;
    if (kCensus) {
        LOG_WARNING("M2 census: '", gpuModel.name,
                    "' h=", tightMax.z - tightMin.z,
                    " top=", tightMax.z,
                    " verts=", model.vertices.size(),
                    " batches=", model.batches.size(),
                    " emitters=", model.particleEmitters.size(),
                    " ribbons=", model.ribbonEmitters.size(),
                    " bones=", model.bones.size(),
                    " fire=", cls.isBrazierOrFire ? 1 : 0,
                    " torch=", cls.isTorch ? 1 : 0,
                    " spellFx=", cls.isSpellEffect ? 1 : 0);
    }
    gpuModel.isTransportDoodad           = cls.isTransportDoodad;
    gpuModel.isBoat                      = cls.isBoat;
    gpuModel.ambientEmitterType          = cls.ambientEmitterType;
    gpuModel.boundMin = tightMin;
    gpuModel.boundMax = tightMax;
    if (gpuModel.isHangingCloth) {
        // Named, once each, so a banner that does not move can be told from a
        // banner this never saw: cloth built into a building's own mesh is not
        // an M2 at all and nothing here can sway it.
        static core::LogBudget clothBudget(12, "Cloth models given a sway");
        if (clothBudget.take()) {
            LOG_INFO("Cloth sway: '", model.name, "' drop=",
                        tightMax.z - tightMin.z, " yards");
        }
    }
    gpuModel.boundRadius = model.boundRadius;
    // Fallback when the M2 header reports 0. Measured from the model origin,
    // like the header value it stands in for: the sphere this feeds is centred
    // there, not on the box. See pipeline/model_bounds.hpp.
    if (gpuModel.boundRadius < 0.01f && !model.vertices.empty()) {
        gpuModel.boundRadius =
            pipeline::modelBoundsOf(model.vertices,
                                    [](const pipeline::M2Vertex& v) {
                                        return v.position;
                                    })
                .radius;
    }
    gpuModel.indexCount = static_cast<uint32_t>(model.indices.size());
    gpuModel.vertexCount = static_cast<uint32_t>(model.vertices.size());

    // Store bone/sequence data for animation
    gpuModel.bones = model.bones;
    gpuModel.sequences = model.sequences;
    gpuModel.globalSequenceDurations = model.globalSequenceDurations;
    gpuModel.lights = model.lights;
    gpuModel.hasAnimation = false;
    for (const auto& bone : model.bones) {
        if (bone.translation.hasData() || bone.rotation.hasData() || bone.scale.hasData()) {
            gpuModel.hasAnimation = true;
            break;
        }
    }

    // Build collision mesh + spatial grid from M2 bounding geometry
    gpuModel.collision.vertices = model.collisionVertices;
    gpuModel.collision.indices = model.collisionIndices;
    gpuModel.collision.build();
    if (gpuModel.collision.valid()) {
        core::Logger::getInstance().debug("  M2 collision mesh: ", gpuModel.collision.triCount,
            " tris, grid ", gpuModel.collision.gridCellsX, "x", gpuModel.collision.gridCellsY);
    }

    // Identify idle variation sequences (animation ID 0 = Stand)
    for (int i = 0; i < static_cast<int>(model.sequences.size()); i++) {
        if (model.sequences[i].id == 0 && model.sequences[i].duration > 0) {
            gpuModel.idleVariationIndices.push_back(i);
        }
    }

    // Batch all GPU uploads (VB, IB, textures) into a single command buffer
    // submission with one fence wait, instead of one fence wait per upload.
    vkCtx_->beginUploadBatch();

    if (hasGeometry) {
        // Create VBO with interleaved vertex data
        // Format: position (3), normal (3), texcoord0 (2), texcoord1 (2), boneWeights (4), boneIndices (4 as float)
        const size_t floatsPerVertex = 18;
        std::vector<float> vertexData;
        vertexData.reserve(model.vertices.size() * floatsPerVertex);

        for (const auto& v : model.vertices) {
            vertexData.push_back(v.position.x);
            vertexData.push_back(v.position.y);
            vertexData.push_back(v.position.z);
            vertexData.push_back(v.normal.x);
            vertexData.push_back(v.normal.y);
            vertexData.push_back(v.normal.z);
            vertexData.push_back(v.texCoords[0].x);
            vertexData.push_back(v.texCoords[0].y);
            vertexData.push_back(v.texCoords[1].x);
            vertexData.push_back(v.texCoords[1].y);
            float w0 = v.boneWeights[0] / 255.0f;
            float w1 = v.boneWeights[1] / 255.0f;
            float w2 = v.boneWeights[2] / 255.0f;
            float w3 = v.boneWeights[3] / 255.0f;
            vertexData.push_back(w0);
            vertexData.push_back(w1);
            vertexData.push_back(w2);
            vertexData.push_back(w3);
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[0], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[1], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[2], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[3], uint8_t(127))));
        }

        // Upload vertex buffer to GPU
        {
            auto buf = uploadBuffer(*vkCtx_,
                vertexData.data(), vertexData.size() * sizeof(float),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            gpuModel.vertexBuffer = buf.buffer;
            gpuModel.vertexAlloc = buf.allocation;
        }

        // Upload index buffer to GPU
        {
            auto buf = uploadBuffer(*vkCtx_,
                model.indices.data(), model.indices.size() * sizeof(uint16_t),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            gpuModel.indexBuffer = buf.buffer;
            gpuModel.indexAlloc = buf.allocation;
        }

        if (!gpuModel.vertexBuffer || !gpuModel.indexBuffer) {
            LOG_ERROR("M2Renderer::loadModel: GPU buffer upload failed for model ", modelId);
        }
    }

    // Load ALL textures from the model into a local vector.
    // textureLoadFailed[i] is true if texture[i] had a named path that failed to load.
    // Such batches are hidden (batchOpacity=0) rather than rendered white.
    std::vector<VkTexture*> allTextures;
    std::vector<bool> textureLoadFailed;
    std::vector<std::string> textureKeysLower;
    if (assetManager) {
        for (size_t ti = 0; ti < model.textures.size(); ti++) {
            const auto& tex = model.textures[ti];
            std::string texPath = tex.filename;
            // Some extracted M2 texture strings contain embedded NUL + garbage suffix.
            // Truncate at first NUL so valid paths like "...foo.blp\0junk" still resolve.
            size_t nul = texPath.find('\0');
            if (nul != std::string::npos) {
                texPath.resize(nul);
            }
            if (!texPath.empty()) {
                std::string texKey = texPath;
                std::replace(texKey.begin(), texKey.end(), '/', '\\');
                std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                VkTexture* texPtr = loadTexture(texPath, tex.flags);
                bool failed = (texPtr == whiteTexture_.get());
                if (failed) {
                    static uint32_t loggedModelTextureFails = 0;
                    static bool loggedModelTextureFailSuppressed = false;
                    if (loggedModelTextureFails < 250) {
                        LOG_WARNING("M2 model ", model.name, " texture[", ti, "] failed to load: ", texPath);
                        ++loggedModelTextureFails;
                    } else if (!loggedModelTextureFailSuppressed) {
                        LOG_WARNING("M2 model texture-failure warnings suppressed after ",
                                    loggedModelTextureFails, " entries");
                        loggedModelTextureFailSuppressed = true;
                    }
                }
                if (isInvisibleTrap) {
                    LOG_INFO("  InvisibleTrap texture[", ti, "]: ", texPath, " -> ", (failed ? "WHITE" : "OK"));
                }
                allTextures.push_back(texPtr);
                textureLoadFailed.push_back(failed);
                textureKeysLower.push_back(std::move(texKey));
            } else {
                if (isInvisibleTrap) {
                    LOG_INFO("  InvisibleTrap texture[", ti, "]: EMPTY (using white fallback)");
                }
                // A slot with no filename is one the model expects someone
                // else to fill: a creature skin from CreatureDisplayInfo
                // (types 11-13), a character component, an item texture. When
                // nothing fills it the batch draws the white fallback, flat
                // and unlit, and the only sign is on screen - which is what
                // "glow cards rendered as white 2D meshes" turned out to be
                // every previous time it was reported.
                //
                // Not an error: plenty of these are filled a moment later by
                // setModelTexture or setTextureSlotOverride, and the renderer
                // cannot see that from here. It is worth naming anyway,
                // because when it is not filled nothing else says so and the
                // model has to be guessed at from a screenshot.
                // Type 0 means the model names its own texture, so an empty
                // name is a broken model - and it draws flat white, which
                // reads as a missing texture rather than as a bad file.
                //
                // This was written the other way round at first, warning for
                // types 11-13, where an empty name is how a creature says its
                // skin comes from CreatureDisplayInfo. It never fired once.
                // Meanwhile the Elemental Slave's white sheets were an
                // imported override with three type-0 slots and no names in
                // them, which this would have found in a single run.
                if (tex.type == 0) {
                    static core::LogBudget unnamedSlotBudget(
                        24, "M2 models with an unnamed texture of their own");
                    if (unnamedSlotBudget.take()) {
                        LOG_WARNING("M2 '", model.name, "' texture[", ti,
                                    "] names no file but is type 0, which means it"
                                    " should - it draws white");
                    }
                }
                allTextures.push_back(whiteTexture_.get());
                textureLoadFailed.push_back(false);  // Empty filename = intentional white (type!=0)
                textureKeysLower.emplace_back();
            }
        }
    }

    static const bool kGlowDiag = core::envFlagEnabled("WOWEE_M2_GLOW_DIAG", false);
    if (kGlowDiag) {
        if (gpuModel.isLanternLike) {
            for (size_t ti = 0; ti < model.textures.size(); ++ti) {
                const std::string key = (ti < textureKeysLower.size()) ? textureKeysLower[ti] : std::string();
                LOG_DEBUG("M2 GLOW TEX '", model.name, "' tex[", ti, "]='", key, "' flags=0x",
                          std::hex, model.textures[ti].flags, std::dec);
            }
        }
    }

    setupEmitterResources(gpuModel, model, allTextures);

    // Copy texture transform data for UV animation
    gpuModel.textureTransforms = model.textureTransforms;
    gpuModel.textureTransformLookup = model.textureTransformLookup;
    gpuModel.colorAlphaTracks = model.colorAlphaTracks;
    gpuModel.textureWeightTracks = model.textureWeightTracks;
    gpuModel.hasTextureAnimation = false;

    // Build per-batch GPU entries
    if (!model.batches.empty()) {
        for (const auto& batch : model.batches) {
            // A submesh that reaches past the model's own indices is not drawn.
            //
            // vkCmdDrawIndexed does not check this and the GPU does not survive
            // it: validation caught a batch starting at index 6,290,784 of a
            // 768-index buffer - an ending offset 25 MB past the end - repeated
            // 46 times over twelve seconds before the device was lost. The same
            // start every time, so it is a submesh read from the wrong offset
            // rather than memory going bad, and this client parses two M2
            // layouts whose skin data does not sit in the same place.
            //
            // Dropping the submesh loses a piece of one doodad. Drawing it
            // loses the device.
            const uint64_t end = static_cast<uint64_t>(batch.indexStart) + batch.indexCount;
            if (end > gpuModel.indexCount) {
                LOG_WARNING("M2 '", gpuModel.name, "': submesh indices ",
                            batch.indexStart, "..", end, " lie past the model's ",
                            gpuModel.indexCount, " - not drawn");
                continue;
            }
            M2ModelGPU::BatchGPU bgpu;
            bgpu.indexStart = batch.indexStart;
            bgpu.indexCount = batch.indexCount;
            bgpu.colorTrackIndex = batch.colorIndex;
            bgpu.priorityPlane = batch.priorityPlane;
            if (batch.transparencyIndex < model.textureWeightLookup.size())
                bgpu.weightTrackIndex = model.textureWeightLookup[batch.transparencyIndex];

            // Store texture animation index from batch
            bgpu.textureAnimIndex = batch.textureAnimIndex;
            if (bgpu.textureAnimIndex != 0xFFFF) {
                gpuModel.hasTextureAnimation = true;
            }

            // Store blend mode and flags from material
            if (batch.materialIndex < model.materials.size()) {
                bgpu.blendMode = model.materials[batch.materialIndex].blendMode;
                bgpu.materialFlags = model.materials[batch.materialIndex].flags;
                if (bgpu.blendMode >= 2) gpuModel.hasTransparentBatches = true;
            }

            // Copy LOD level from batch
            bgpu.submeshLevel = batch.submeshLevel;

            // Resolve texture: batch.textureIndex → textureLookup → allTextures
            VkTexture* tex = whiteTexture_.get();
            bool texFailed = false;
            std::string batchTexKeyLower;
            if (batch.textureIndex < model.textureLookup.size()) {
                uint16_t texIdx = model.textureLookup[batch.textureIndex];
                if (texIdx < allTextures.size()) {
                    tex = allTextures[texIdx];
                    texFailed = (texIdx < textureLoadFailed.size()) && textureLoadFailed[texIdx];
                    if (texIdx < textureKeysLower.size()) {
                        batchTexKeyLower = textureKeysLower[texIdx];
                    }
                }
                if (texIdx < model.textures.size()) {
                    bgpu.texFlags = static_cast<uint8_t>(model.textures[texIdx].flags & 0x3);
                }
            } else if (!allTextures.empty()) {
                LOG_WARNING("M2 '", model.name, "' batch textureIndex ", batch.textureIndex,
                            " out of range (textureLookup size=", model.textureLookup.size(),
                            ") - falling back to texture[0]");
                tex = allTextures[0];
                texFailed = !textureLoadFailed.empty() && textureLoadFailed[0];
                if (!textureKeysLower.empty()) {
                    batchTexKeyLower = textureKeysLower[0];
                }
            }

            if (texFailed && groundDetailModel) {
                static const std::string kDetailFallbackTexture = "World\\NoDXT\\Detail\\8des_detaildoodads01.blp";
                VkTexture* fallbackTex = loadTexture(kDetailFallbackTexture, 0);
                if (fallbackTex != nullptr && fallbackTex != whiteTexture_.get()) {
                    tex = fallbackTex;
                    texFailed = false;
                }
            }
            bgpu.texture = tex;
            const auto tcls = classifyBatchTexture(batchTexKeyLower);
            bgpu.starLayer = tcls.starPointLayer;
            const bool modelLanternFamily = gpuModel.isLanternLike;
            const bool torchGlowCard = gpuModel.isTorch &&
                tcls.hasGlowToken && tcls.hasGlowCardToken;
            // Fire pits and braziers commonly pair an authored flame mesh
            // (for example FLAMELICKSMALL) with a separate flat GLOW32 card.
            // Replace only that glow-textured card; retaining the flame mesh
            // preserves the intended animated fire shape.
            const bool fireGlowCard = gpuModel.isBrazierOrFire &&
                tcls.hasGlowToken && tcls.hasGlowCardToken;
            bgpu.lanternGlowHint =
                tcls.softGlowSurface ||
                tcls.exactLanternGlowTex ||
                torchGlowCard ||
                fireGlowCard ||
                ((tcls.hasGlowToken || (modelLanternFamily && tcls.hasFlameToken)) &&
                 (tcls.lanternFamily || modelLanternFamily) &&
                 (!tcls.likelyFlame || modelLanternFamily));
            bgpu.glowCardLike = bgpu.lanternGlowHint &&
                (tcls.hasGlowCardToken || tcls.softGlowSurface);
            // A flame texture is the fire itself, not a flat card standing in for
            // one, so swapping it for a featureless sprite deletes the visible
            // flame - chandeliers and candelabra lit their surroundings while
            // their candles sat unlit. Braziers already keep their flame mesh for
            // this reason (see fireGlowCard above); FLAMELICK counts as a
            // glow-card token, so lantern-family models were not getting the same
            // treatment. Keep the mesh and let the sprite glow behind it.
            const bool flameCard = tcls.hasFlameToken || tcls.likelyFlame;
            bgpu.preserveGlowMesh = tcls.softGlowSurface ||
                                    (bgpu.lanternGlowHint && flameCard);
            bgpu.glowTint = tcls.glowTint;
            if (tex != nullptr && tex != whiteTexture_.get()) {
                applyBatchTextureProps(bgpu);
            }
            // How its stages combine and where their coordinates come from,
            // as the client works it out at skin load. The second stage was
            // used for one case only - shader 0, two textures, a blended
            // layer, as an RGBA mask - so armour lost its sphere-mapped shine
            // and two-texture flames their doubling, about 5,500 batches.
            {
                const auto coordAt = [&](size_t i) -> int16_t {
                    return i < model.textureCoordLookup.size() ? model.textureCoordLookup[i] : 0;
                };
                const M2BatchShading shading = m2BatchShading(
                    model.globalFlags, model.textureCombinerCombos, batch.shader, batch.textureCount,
                    bgpu.blendMode, coordAt(batch.textureUnit), coordAt(static_cast<size_t>(batch.textureUnit) + 1));
                bgpu.combiner = shading.combiner;
                bgpu.textureUnit = m2StageCoordsPush(shading.coords0);
                bgpu.secondTexCoords = m2StageCoordsPush(shading.coords1);
                bgpu.shaderDrawn = shading.drawn;
                if (shading.stages == 2 && m2CombinerUsesSecondStage(shading.combiner)) {
                    const size_t second = static_cast<size_t>(batch.textureIndex) + 1;
                    const uint16_t tex = second < model.textureLookup.size() ? model.textureLookup[second] : UINT16_MAX;
                    if (tex < allTextures.size() && tex < textureLoadFailed.size() && !textureLoadFailed[tex]) {
                        bgpu.secondTexture = allTextures[tex];
                    } else {
                        // Drawn with its second stage white: the first alone.
                        static std::unordered_set<std::string> saidMissing;
                        if (saidMissing.insert(model.name).second) {
                            LOG_WARNING("M2 '", model.name, "' batch ", gpuModel.batches.size(),
                                        ": its second texture (lookup ", second, ") is not loaded; drawn without it");
                        }
                    }
                    if (tex < model.textures.size())
                        bgpu.secondTexFlags = static_cast<uint8_t>(model.textures[tex].flags & 0x3);
                }
            }

            // Hide the batch if its texture failed to load, and remember what it
            // was waiting for: retryPendingBatchTextures shows it if a later
            // attempt succeeds, rather than leaving a hole until the model is
            // unloaded.
            const bool hiddenForTexture = texFailed && !groundDetailModel;
            if (hiddenForTexture && batch.textureIndex < model.textureLookup.size()) {
                const uint16_t texIdx = model.textureLookup[batch.textureIndex];
                if (texIdx < model.textures.size()) {
                    bgpu.pendingTextureKey = batchTexKeyLower;
                    bgpu.pendingTextureFlags = model.textures[texIdx].flags;
                }
            }

            // And say so, because invisible is indistinguishable from absent.
            //
            // A tree reported as missing its inner bark, with the top floating
            // over a transparent gap, is one batch of two: the trunk drawn at
            // zero opacity while the canopy draws normally. Nothing named it.
            // The texture loader logs where a file fails, but a batch turned
            // invisible three steps later on the strength of that flag said
            // nothing at all, so the search went to the texture files - which
            // were all present and correct - instead of to the batch.
            if (hiddenForTexture) {
                static core::LogBudget hiddenBatchBudget(
                    16, "M2 batches hidden because their texture did not load");
                if (hiddenBatchBudget.take()) {
                    LOG_WARNING("M2 batch drawn invisible: '", model.name, "' batch ",
                                gpuModel.batches.size(), " wanted '", batchTexKeyLower,
                                "' and did not get it");
                }
            }

            // The batch's colour alpha and texture weight: ghosts, ethereal
            // effects, fading doodads, a spell's emitter mesh. One that never
            // changes is applied here, a zero included - that layer is not
            // drawn. One that animates is sampled on each instance's clock
            // when drawn; taking any one of its keys here left it at that
            // value for good, and an emitter mesh meant to show for a moment
            // stood there solid. The sky reads its own on the day clock.
            {
                if (batch.colorIndex < model.colorRGB.size()) {
                    // The batch's authored colour. A glow card is painted
                    // white and coloured here, so without it every fire in the
                    // world burns white: Orgrimmar's carries (1.0, 0.329, 0.0).
                    bgpu.tint = model.colorRGB[batch.colorIndex];
                }
                float atRest = 1.0f;
                if (!skyMode_) {
                    const M2BatchOpacity opacity = m2BatchOpacity(
                        gpuModel.colorAlphaTracks, bgpu.colorTrackIndex,
                        gpuModel.textureWeightTracks, bgpu.weightTrackIndex);
                    bgpu.animatedOpacity = opacity.animated;
                    atRest = opacity.atRest;
                }
                bgpu.blendedByOpacity = bgpu.animatedOpacity || atRest < kM2BatchOpaqueOpacity;
                if (bgpu.blendedByOpacity) gpuModel.hasTransparentBatches = true;
                bgpu.loadedOpacity = atRest;
                bgpu.batchOpacity = hiddenForTexture ? 0.0f : atRest;
            }

            // Compute batch center and radius for glow sprite positioning
            if ((bgpu.blendMode >= 3 || bgpu.glowCardLike) && batch.indexCount > 0) {
                glm::vec3 sum(0.0f);
                uint32_t counted = 0;
                std::unordered_map<uint16_t, glm::vec4> boneAnchorSums;
                for (uint32_t j = batch.indexStart; j < batch.indexStart + batch.indexCount; j++) {
                    if (j < model.indices.size()) {
                        uint16_t vi = model.indices[j];
                        if (vi < model.vertices.size()) {
                            const auto& vertex = model.vertices[vi];
                            sum += vertex.position;
                            for (size_t influence = 0; influence < 4; ++influence) {
                                const float weight = static_cast<float>(vertex.boneWeights[influence]) / 255.0f;
                                if (weight <= 0.0f) continue;
                                const uint16_t bone = vertex.boneIndices[influence];
                                boneAnchorSums[bone] += glm::vec4(vertex.position * weight, weight);
                            }
                            counted++;
                        }
                    }
                }
                if (counted > 0) {
                    bgpu.center = sum / static_cast<float>(counted);
                    bgpu.lightBoneAnchors.reserve(boneAnchorSums.size());
                    for (const auto& [bone, weightedPoint] : boneAnchorSums) {
                        bgpu.lightBoneAnchors.push_back({
                            .bone = bone, .weightedPoint = weightedPoint / static_cast<float>(counted)});
                    }
                    if (!boneAnchorSums.empty() && !model.bones.empty()) {
                        const auto dominant = std::max_element(
                            boneAnchorSums.begin(), boneAnchorSums.end(),
                            [](const auto& a, const auto& b) {
                                return a.second.w < b.second.w;
                            });
                        size_t suspensionBone = dominant->first;
                        while (suspensionBone < model.bones.size()) {
                            const int16_t parent = model.bones[suspensionBone].parentBone;
                            if (parent < 0 || static_cast<size_t>(parent) >= model.bones.size()) break;
                            const glm::vec3 span = model.bones[parent].pivot -
                                                   model.bones[suspensionBone].pivot;
                            // A hanging chain climbs through parents above the
                            // bulb. Stop before a generic model/root bone at the
                            // placement origin poisons the projection direction.
                            if (span.z <= 0.01f || glm::length(span) > 5.0f) break;
                            suspensionBone = static_cast<size_t>(parent);
                        }
                        if (suspensionBone < model.bones.size() &&
                            suspensionBone != dominant->first) {
                            bgpu.lightSuspensionBone = static_cast<uint16_t>(suspensionBone);
                            bgpu.lightSuspensionPoint = model.bones[suspensionBone].pivot;
                        }
                    }
                    float maxDist = 0.0f;
                    for (uint32_t j = batch.indexStart; j < batch.indexStart + batch.indexCount; j++) {
                        if (j < model.indices.size()) {
                            uint16_t vi = model.indices[j];
                            if (vi < model.vertices.size()) {
                                float d = glm::length(model.vertices[vi].position - bgpu.center);
                                maxDist = std::max(maxDist, d);
                            }
                        }
                    }
                    bgpu.glowSize = std::max(maxDist, 0.5f);
                    // Upright fire glow cards are deliberately tall, so their
                    // geometric center floats above a small ground fire. Keep
                    // the radial halo near the fuel/flame base instead.
                    if (fireGlowCard && gpuModel.isGroundFire) {
                        // Ground-fire sprites follow a particle emitter and its
                        // animated bone at render time. Origin is the fallback
                        // for unusual fire models without an emitter.
                        bgpu.center = glm::vec3(0.0f);
                    }
                }
            }

            // Why a batch of a named model came out the way it did.
            //
            // Set WOWEE_M2_BATCH_DIAG to a substring of the model's name and
            // every batch of every model matching it is printed once, as it is
            // built. A batch that never reaches the screen leaves no other
            // trace: the values that decide its fate are read here and then
            // only compared, so a mesh that vanishes looks the same from the
            // outside as one that was never in the file.
            static const std::string kBatchDiag = [] {
                const char* v = std::getenv("WOWEE_M2_BATCH_DIAG");
                std::string s = v ? v : "";
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return s;
            }();
            if (!kBatchDiag.empty()) {
                std::string lowerName = model.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerName.find(kBatchDiag) != std::string::npos) {
                    // At warning, because the log carries nothing below it.
                    // Setting WOWEE_M2_BATCH_DIAG is asking for these lines,
                    // and they were being written where nobody could read
                    // them - which is the same as not writing them.
                    LOG_WARNING("M2 BATCH '", model.name, "' #", gpuModel.batches.size(),
                             ": tex='", batchTexKeyLower,
                             "' blend=", static_cast<int>(bgpu.blendMode),
                             " matFlags=0x", std::hex, bgpu.materialFlags, std::dec,
                             " alphaTestWillBe=",
                             m2BatchNeedsAlphaTest(bgpu.blendMode, bgpu.hasAlpha) ? 1 : 0,
                             " hasAlpha=", bgpu.hasAlpha ? "Y" : "N",
                             " alphaIsSilhouette=", bgpu.alphaIsSilhouette ? "Y" : "N",
                             " glowCardLike=", bgpu.glowCardLike ? "Y" : "N",
                             " preserveGlowMesh=", bgpu.preserveGlowMesh ? "Y" : "N",
                             " opacity=", bgpu.batchOpacity,
                             " idxCount=", bgpu.indexCount,
                             " texFailed=", texFailed ? "Y" : "N");
                }
            }

            // Optional diagnostics for glow/light batches (disabled by default).
            if (kGlowDiag && gpuModel.isLanternLike) {
                LOG_DEBUG("M2 GLOW DIAG '", model.name, "' batch ", gpuModel.batches.size(),
                          ": blend=", bgpu.blendMode, " matFlags=0x",
                          std::hex, bgpu.materialFlags, std::dec,
                          " hasAlpha=", bgpu.hasAlpha ? "Y" : "N",
                          " unlit=", (bgpu.materialFlags & 0x01) ? "Y" : "N",
                          " lanternHint=", bgpu.lanternGlowHint ? "Y" : "N",
                          " glowSize=", bgpu.glowSize,
                          " tex=", bgpu.texture,
                          " idxCount=", bgpu.indexCount);
            }
            gpuModel.batches.push_back(bgpu);
        }
    } else if (gpuModel.indexCount > 0) {
        // The client draws batches and nothing else, so a skin with geometry
        // and no batches draws nothing - an invisible stalker's does exactly
        // that. It used to be drawn whole with the first texture.
        LOG_INFO("M2 '", model.name, "' has ", gpuModel.indexCount,
                 " indices and no batches: it draws nothing but its emitters");
    }

    gpuModel.materialDrawOrder = m2MaterialDrawOrder(gpuModel.batches);

    // Detect particle emitter volume models: box mesh (24 verts, 36 indices)
    // with disproportionately large bounds. These are invisible bounding volumes
    // that only exist to spawn particles - their mesh should never be rendered.
    if (!isInvisibleTrap && !groundDetailModel &&
        gpuModel.vertexCount <= 24 && gpuModel.indexCount <= 36
        && !model.particleEmitters.empty()) {
        glm::vec3 size = gpuModel.boundMax - gpuModel.boundMin;
        float maxDim = std::max({size.x, size.y, size.z});
        if (maxDim > 5.0f) {
            gpuModel.isInvisibleTrap = true;
            LOG_DEBUG("M2 emitter volume hidden: '", model.name, "' size=(",
                      size.x, " x ", size.y, " x ", size.z, ")");
        }
    }

    vkCtx_->endUploadBatch();

    // Allocate Vulkan descriptor sets and UBOs for each batch
    for (auto& bgpu : gpuModel.batches) {
        // Create combined UBO for M2Params (binding 1) + M2Material (binding 2)
        // We allocate them as separate buffers for clarity
        VmaAllocationInfo matAllocInfo{};
        {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = sizeof(M2MaterialUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &bgpu.materialUBO, &bgpu.materialUBOAlloc, &matAllocInfo);

            bgpu.materialUBOMapped = matAllocInfo.pMappedData;
            writeBatchMaterial(gpuModel, bgpu);
            const auto& mat = *static_cast<const M2MaterialUBO*>(bgpu.materialUBOMapped);

            // What the sky model's layers are actually being given, once each.
            //
            // Three fixes have been aimed at this by reading - the colour key,
            // the alpha test's screen-space rescale, the order of the sky
            // early-out - and the flicker survived all three, which means the
            // reading was wrong about which of these values the sky's blended
            // layers carry. Twenty-three of them is too many to hold in the
            // head, and only this says what they are.
            if (skyMode_) {
                LOG_INFO("skyM2 batch ", &bgpu - gpuModel.batches.data(),
                         ": blend=", static_cast<int>(bgpu.blendMode),
                         " alphaTest=", mat.alphaTest,
                         " hasAlpha=", bgpu.hasAlpha ? 1 : 0,
                         " unlit=", mat.unlit,
                         " glowCardLike=", bgpu.glowCardLike ? 1 : 0,
                         " lanternHint=", bgpu.lanternGlowHint ? 1 : 0,
                         " preserveGlowMesh=", bgpu.preserveGlowMesh ? 1 : 0,
                         " texAnim=", bgpu.textureAnimIndex,
                         " tint=(", bgpu.tint.r, ",", bgpu.tint.g, ",", bgpu.tint.b, ")");
            }
        }

        // Allocate descriptor set and write all bindings
        bgpu.materialSet = allocateMaterialSet();
        // Valid, not merely non-null - the same shape as the particle and
        // ribbon sets below. descriptorInfo() returns the texture's handles as
        // they are, so one whose upload or view creation failed writes
        // VK_NULL_HANDLE into a live descriptor and declares
        // SHADER_READ_ONLY_OPTIMAL over it, which reaches an NVIDIA driver as a
        // graphics engine exception and a lost device. See #123.
        //
        // The set is dropped when even the white fallback is unsampleable, and
        // the render passes already skip a batch with no set: losing a batch
        // costs part of a model, and a null image view costs the device.
        VkTexture* batchTex = (bgpu.texture && bgpu.texture->isValid())
            ? bgpu.texture : whiteTexture_.get();
        if (!batchTex || !batchTex->isValid()) bgpu.materialSet = VK_NULL_HANDLE;
        writeBatchDescriptors(bgpu);
    }

    // Pre-compute available LOD levels to avoid per-instance batch iteration
    gpuModel.availableLODs = 0;
    for (const auto& b : gpuModel.batches) {
        if (b.submeshLevel < 8) gpuModel.availableLODs |= (1u << b.submeshLevel);
    }

    for (size_t bi = 0; bi < gpuModel.batches.size(); ++bi) {
        if (!gpuModel.batches[bi].pendingTextureKey.empty())
            pendingBatchTextures_.push_back({modelId, bi});
    }

    registerRtModel(gpuModel, model);
    // No rebuild of the spatial index for this. It used to be marked dirty
    // here, against the map rehashing under the instances' cachedModel
    // pointers - but an unordered_map's rehash moves no elements, so the
    // pointers stand, and an existing id is assigned into the node it already
    // has. The rebuild walked every instance in the world on the next update,
    // for every model that loaded: 26-53 ms each time something new came into
    // view in Dalaran.
    models[modelId] = std::move(gpuModel);

    LOG_DEBUG("Loaded M2 model: ", model.name, " (", models[modelId].vertexCount, " vertices, ",
              models[modelId].indexCount / 3, " triangles, ", models[modelId].batches.size(), " batches)");

    retryPendingBatchTextures();

    return true;
}

} // namespace rendering
} // namespace wowee

namespace wowee {
namespace rendering {

void M2Renderer::registerRtModel(M2ModelGPU& gpuModel, const pipeline::M2Model& model) {
    if (!rtScene_ || model.vertices.empty() || model.indices.empty()) return;
    // What the renderer never draws, what is too small and numerous to be
    // worth its triangles (ground clutter), and what moves: an animated model
    // would cast its bind pose, not the pose on screen.
    if (gpuModel.isInvisibleTrap || gpuModel.isSpellEffect ||
        gpuModel.isGroundDetail || gpuModel.isSkyBird || gpuModel.isLightBeam) {
        return;
    }
    if (gpuModel.hasAnimation && !gpuModel.disableAnimation && !gpuModel.isTransportDoodad) return;

    RtScene::MeshSource src;
    src.positions.reserve(model.vertices.size());
    for (const auto& v : model.vertices) src.positions.push_back(v.position);
    for (const auto& batch : gpuModel.batches) {
        // A layer that fades in and out is drawn blended, and blended layers
        // cast no shadow.
        if (batch.submeshLevel != 0 || batch.blendedByOpacity || !batch.shaderDrawn ||
            !m2BatchCastsShadow(batch.blendMode, batch.batchOpacity,
                                batch.glowCardLike, batch.starLayer)) {
            continue;
        }
        glm::vec3 albedo = batch.tint;
        float opacity = 1.0f;
        if (batch.texture) {
            albedo *= batch.texture->averageColor();
            if (m2BatchNeedsAlphaTest(static_cast<uint8_t>(batch.blendMode), batch.hasAlpha)) {
                opacity = batch.texture->alphaCoverage();
            }
        }
        const float surface = packRtSurface(albedo, opacity);
        const size_t end = std::min<size_t>(size_t(batch.indexStart) + batch.indexCount,
                                            model.indices.size());
        for (size_t i = batch.indexStart; i + 2 < end; i += 3) {
            src.indices.push_back(model.indices[i]);
            src.indices.push_back(model.indices[i + 1]);
            src.indices.push_back(model.indices[i + 2]);
            src.surfaces.push_back(surface);
        }
    }
    gpuModel.rtMesh = rtScene_->addMesh(std::move(src));
}

void M2Renderer::releaseRtModel(M2ModelGPU& gpuModel) {
    if (!rtScene_ || gpuModel.rtMesh == RtScene::kInvalid) return;
    // Instances of it are normally gone already; any left are dropped here,
    // and the M2Instance that still names one re-registers on the next sync
    // if its model comes back.
    for (size_t i = 0; i < rtOwned_.size();) {
        const uint32_t id = rtOwned_[i];
        if (rtMeshOf_[id] == gpuModel.rtMesh) {
            rtScene_->removeInstance(id);
            rtSeen_[id] = 0;
            rtOwned_[i] = rtOwned_.back();
            rtOwned_.pop_back();
        } else {
            ++i;
        }
    }
    rtScene_->removeMesh(gpuModel.rtMesh);
    gpuModel.rtMesh = RtScene::kInvalid;
}

void M2Renderer::syncRtScene() {
    if (!rtScene_ || !rtScene_->isActive()) return;
    const uint64_t gen = ++rtSyncGeneration_;
    auto track = [&](uint32_t id, uint32_t mesh) {
        if (id >= rtSeen_.size()) {
            rtSeen_.resize(id + 1, 0);
            rtMeshOf_.resize(id + 1, RtScene::kInvalid);
        }
        rtSeen_[id] = gen;
        rtMeshOf_[id] = mesh;
    };
    for (auto& inst : instances) {
        auto it = models.find(inst.modelId);
        const uint32_t mesh = it != models.end() ? it->second.rtMesh : RtScene::kInvalid;
        const bool wanted = mesh != RtScene::kInvalid && inst.fade >= 1.0f;
        // Still ours, not claimed by another instance this pass (a copied
        // M2Instance carries its original's id), and placing the same mesh.
        const bool valid = inst.rtInstance != RtScene::kInvalid &&
                           inst.rtInstance < rtSeen_.size() && rtSeen_[inst.rtInstance] != 0 &&
                           rtSeen_[inst.rtInstance] != gen && rtMeshOf_[inst.rtInstance] == mesh;
        if (!wanted) {
            inst.rtInstance = RtScene::kInvalid;  // an unseen id is removed below
            continue;
        }
        if (!valid) {
            inst.rtInstance = rtScene_->addInstance(mesh, inst.modelMatrix);
            if (inst.rtInstance == RtScene::kInvalid) continue;
            inst.rtMatrix = inst.modelMatrix;
            rtOwned_.push_back(inst.rtInstance);
            track(inst.rtInstance, mesh);
            continue;
        }
        if (inst.rtMatrix != inst.modelMatrix) {
            rtScene_->setInstanceTransform(inst.rtInstance, inst.modelMatrix);
            inst.rtMatrix = inst.modelMatrix;
        }
        track(inst.rtInstance, mesh);
    }
    for (size_t i = 0; i < rtOwned_.size();) {
        const uint32_t id = rtOwned_[i];
        if (rtSeen_[id] != gen) {
            rtScene_->removeInstance(id);
            rtSeen_[id] = 0;
            rtOwned_[i] = rtOwned_.back();
            rtOwned_.pop_back();
        } else {
            ++i;
        }
    }
}

} // namespace rendering
} // namespace wowee
