#include "collision/collision_world.hpp"
#include "collision/geometry.hpp"
#include "pipeline/heap_bytes.hpp"
#include "rendering/face_culling.hpp"
#include "rendering/pom_quality.hpp"
#include "pipeline/placement_transform.hpp"
#include "collision/movement_world.hpp"
#include "rendering/wmo_vertex.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/wmo_renderer.hpp"
#include <stdexcept>
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/wmo_batch_merge.hpp"
#include "wmo_model_prep.hpp"
#include "rendering/wmo_lighting.hpp"
#include <cstdlib>
#include "rendering/normal_map.hpp"
#include "rendering/texture_upload.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/water_reflection.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <thread>
#include <unordered_set>

namespace wowee {
namespace rendering {


namespace {

/// How far a banner hanging `clothDrop` units below its bar can move from
/// where its vertices sit, in the building's units. wmo.vert.glsl swings the
/// hem by up to 5% of the drop, times clothWindSway's reach: along the wind
/// at most 2 x 0.3 lean + 0.95 x 2.5 swing, across it 0.8 x 2.5 and in and
/// out 0.3 (WindField caps the lean below 0.3 and the swing at 2.5), which is
/// under 3.6 together.
float clothSwayReach(float clothDrop) {
    return clothDrop * 0.05f * 3.6f;
}
/// The strength a building's normal map is derived with (Sobel gradient
/// scale), and so the variant its cache key carries: the character renderer
/// derives from the same files at another strength.
constexpr float kWmoNormalMapStrength = 2.0f;
constexpr uint8_t kWmoNormalMapStrengthKey = static_cast<uint8_t>(kWmoNormalMapStrength);

// A WMO group winds its outside counter-clockwise against MONR, as an M2 does:
// 832638 of 833851 drawn triangles over 300 stock groups. Placement is
// rotation and a positive scale, so it arrives counter-clockwise. Under the
// builder's default of clockwise every outer wall reached wmo.frag as a back
// face and was lit with its normal turned inward - the sunlit side dark and
// the shaded side lit.
//
// The water reflection draws through these same pipelines from a camera under
// the water looking up (mirroredView): a view like any other, not a mirror, so
// its triangles wind as the camera's do; the water flips the picture when it
// samples it. That pass only draws the scene with multisampling off.
constexpr VkFrontFace kWmoFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

// What wmo_shadow.vert is pushed: the map's matrix and the call's first
// caster place.
struct WmoShadowPush {
    glm::mat4 lightSpace{1.0f};
    glm::uvec4 call{0u};
};
static_assert(sizeof(WmoShadowPush) <= 128, "the push block every device must take");
} // namespace

using collision::rayIntersectsAABB;
using collision::transformAABB;

// A group has at most 65536 vertices (16-bit indices); a page holds dozens.
WMORenderer::WMORenderer()
    : geometry_({.what = "WMO", .vertexStride = sizeof(WMOVertex), .indexType = VK_INDEX_TYPE_UINT16,
                 .pageVertices = 1u << 18u, .pageIndices = 1u << 20u}) {}

WMORenderer::~WMORenderer() {
    shutdown();
}

/// Builds the four main-pass pipelines from an already-loaded shader pair.
///
/// initialize() and recreatePipelines() both need exactly these four, in
/// exactly these states, and each described all four for itself. They only
/// differ in what happens when one fails to build: the first cannot continue,
/// the second is a rebuild after a settings change and leaves the renderer
/// with whatever it managed.
bool WMORenderer::buildMainPassPipelines(VkDevice device,
                                         wowee::rendering::VkShaderModule& vertShader,
                                         wowee::rendering::VkShaderModule& fragShader) {
    // --- Vertex input ---
    const VkVertexInputBindingDescription vertexBinding =
        perVertexBinding(sizeof(WMOVertex));
    const std::vector<VkVertexInputAttributeDescription> vertexAttribs =
        toVkAttributes(kWmoVertexAttributes);

    // The fragment shader with its alpha test compiled in (kAlphaTestPipeline).
    // Every pipeline but the opaque one takes it, so they draw as before; the
    // opaque one leaves it out so that the shader cannot discard at all.
    const VkBool32 alphaTestOn = VK_TRUE;
    const VkSpecializationMapEntry alphaTestEntry{.constantID = 0, .offset = 0,
                                                  .size = sizeof(VkBool32)};
    const VkSpecializationInfo alphaTestSpec{.mapEntryCount = 1, .pMapEntries = &alphaTestEntry,
                                             .dataSize = sizeof(VkBool32), .pData = &alphaTestOn};
    VkPipelineShaderStageCreateInfo alphaTestFrag = fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);
    alphaTestFrag.pSpecializationInfo = &alphaTestSpec;

    // --- Build opaque pipeline (base for derivatives - shared state optimization) ---
    VkRenderPass mainPass = vkCtx_->getImGuiRenderPass();

    opaquePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, kWmoFrontFace)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(faceCullingDynamicStates(*vkCtx_))
        .setFlags(VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)
        .build(device, vkCtx_->getPipelineCache());

    if (!opaquePipeline_) {
        core::Logger::getInstance().error("WMORenderer: failed to create opaque pipeline");
        return false;
    }

    // --- Build alpha-test pipeline (derivative - opaque state, shader can discard) ---
    alphaTestPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), alphaTestFrag)
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, kWmoFrontFace)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(faceCullingDynamicStates(*vkCtx_))
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());

    if (!alphaTestPipeline_) {
        core::Logger::getInstance().error("WMORenderer: alpha-test pipeline not available");
    }

    // --- Build transparent pipeline (derivative of opaque) ---
    transparentPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), alphaTestFrag)
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, kWmoFrontFace)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(faceCullingDynamicStates(*vkCtx_))
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());

    if (!transparentPipeline_) {
        core::Logger::getInstance().warning("WMORenderer: transparent pipeline not available");
    }

    // --- Build wireframe pipeline (derivative of opaque) ---
    wireframePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), alphaTestFrag)
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE, kWmoFrontFace)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(faceCullingDynamicStates(*vkCtx_))
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());

    if (!wireframePipeline_) {
        core::Logger::getInstance().warning("WMORenderer: wireframe pipeline not available");
    }

    return opaquePipeline_ != VK_NULL_HANDLE;
}

bool WMORenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                              pipeline::AssetManager* assets) {
    if (initialized_) { assetManager = assets; return true; }
    core::Logger::getInstance().info("Initializing WMO renderer (Vulkan)...");

    vkCtx_ = ctx;
    assetManager = assets;

    if (!vkCtx_) {
        core::Logger::getInstance().error("WMORenderer: null VkContext");
        return false;
    }
    geometry_.initialize(*vkCtx_);

    const unsigned hc = std::thread::hardware_concurrency();
    const size_t availableCores = (hc > 1u) ? static_cast<size_t>(hc - 1u) : 1ull;
    // WMO culling is lighter than animation; keep defaults conservative to reduce spikes.
    const size_t defaultCullThreads = std::max<size_t>(1, availableCores / 4);
    numCullThreads_ = static_cast<uint32_t>(std::max<size_t>(
        1, envSizeOrDefault("WOWEE_WMO_CULL_THREADS", defaultCullThreads)));
    core::Logger::getInstance().info("WMO cull threads: ", numCullThreads_);

    // Off unless asked for: a building that is not drawn is a worse fault than
    // a building drawn when it did not need to be.
    if (const char* v = std::getenv("WOWEE_WMO_CULL"); v && *v && *v != '0') {
        cullingEnabled_ = true;
        core::Logger::getInstance().info("WMO culling enabled by WOWEE_WMO_CULL");
    }

    VkDevice device = vkCtx_->getDevice();

    // Set 1 is the device's texture table (TextureTable); a batch's textures
    // are handles in its material row. Set 2 is the material table, read by
    // the vertex shader (cloth) and the fragment shader, and what the cull
    // wrote for the draws.
    materialTable_.initialize(*vkCtx_);
    layout_.initialize(*vkCtx_);
    if (!drawPass_.initialize(*vkCtx_)) return false;
    {
        // Binding 0 the material table; what the cull wrote: 1 the draws'
        // records, 2 the groups' lights; 3 the group table, 4 the instance
        // table (wmo.vert).
        std::vector<VkDescriptorSetLayoutBinding> bindings(5);
        for (uint32_t b = 0; b < 5; ++b)
            bindings[b] = {.binding = b, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                           .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | (b == 0 ? VkShaderStageFlags(VK_SHADER_STAGE_FRAGMENT_BIT) : VkShaderStageFlags(0))};
        materialSetLayout_ = createDescriptorSetLayout(device, bindings);
        const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 10};
        VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        if (!materialSetLayout_ || vkCreateDescriptorPool(device, &poolInfo, nullptr, &materialDescPool_) != VK_SUCCESS) {
            LOG_ERROR("WMORenderer: could not make the material table's set layout or pool; no buildings are drawn");
            return false;
        }
        const VkDescriptorSetLayout layouts[2] = {materialSetLayout_, materialSetLayout_};
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = materialDescPool_;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        if (vkCtx_->allocateDescriptorSets(ai, materialSet_) != VK_SUCCESS) {
            LOG_ERROR("WMORenderer: could not allocate the material table's sets; no buildings are drawn");
            return false;
        }
    }

    // --- Create pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(WMOPushConstants);

    std::vector<VkDescriptorSetLayout> setLayouts = {perFrameLayout, vkCtx_->textureTable().layout(),
                                                     materialSetLayout_};
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(vkCtx_->getPhysicalDevice(), &properties);
        maxDrawsPerCall_ = std::max(1u, properties.limits.maxDrawIndirectCount);
    }
    pipelineLayout_ = createPipelineLayout(device, setLayouts, { pushRange });
    if (!pipelineLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create pipeline layout");
        return false;
    }

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/wmo.vert.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/wmo.frag.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load fragment shader");
        return false;
    }

    if (!buildMainPassPipelines(device, vertShader, fragShader)) {
        vertShader.destroy();
        fragShader.destroy();
        return false;
    }

    vertShader.destroy();
    fragShader.destroy();

    // --- Create fallback white texture ---
    const uint8_t whitePixel[4] = {255, 255, 255, 255};
    whiteTexture_ = adoptPixels(*vkCtx_, TextureForm::Owned, whitePixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                false, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    // --- Create flat normal placeholder texture ---
    // (128,128,255,128) = flat normal pointing up (0,0,1), mid-height
    const uint8_t flatNormalPixel[4] = {128, 128, 255, 128};
    flatNormalTexture_ = adoptPixels(*vkCtx_, TextureForm::Owned, flatNormalPixel, 1, 1,
                                     VK_FORMAT_R8G8B8A8_UNORM, false, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    modelCacheLimit_ = envSizeMBOrDefault("WOWEE_WMO_MODEL_LIMIT", 4000);
    core::Logger::getInstance().info("WMO model cache limit: ", modelCacheLimit_);

    core::Logger::getInstance().info("WMO renderer initialized (Vulkan)");
    initialized_ = true;
    return true;
}

void WMORenderer::shutdown() {
    scene_->rtChanges.clear(rtScene_);
    scene_->slots.clear();
    core::Logger::getInstance().info("Shutting down WMO renderer...");

    // Without a context there is nothing to free on the GPU and nothing to drain
    // - and nothing to call it on either, which is what this used to try.
    if (!vkCtx_) {
        loadedModels.clear();
        scene_->loadingModels_.clear();
        preparedHere_.clear();
        scene_->instances.clear();
        if (collision_) collision_->clearWmo();
        scene_->instanceIndexById.clear();
        initialized_ = false;
        return;
    }

    VkDevice device = vkCtx_->getDevice();

    vkCtx_->waitIdle();

    // Free all GPU resources for loaded models
    for (auto& [id, model] : loadedModels) {
        releaseRtModel(id);
        for (auto& group : model.groups) {
            destroyGroupGPU(group);
        }
    }

    // Incremental loads also own uploaded groups, even before publication.
    // Closing during city streaming must release those buffers as well.
    for (auto& [id, model] : scene_->loadingModels_) {
        for (auto& group : model.groups) destroyGroupGPU(group);
    }
    scene_->loadingModels_.clear();
    preparedHere_.clear();

    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;

    // Give back the fallbacks; the models' own textures go with the models.
    whiteTexture_.reset();
    flatNormalTexture_.reset();

    loadedModels.clear();
    scene_->instances.clear();
    if (collision_) collision_->clearWmo();
    scene_->instanceIndexById.clear();

    // destroyGroupGPU defers its frees. Drain them here, before the tables
    // they give rows back to go: no further frames will run to drain the
    // queue.
    vkCtx_->flushDeferredCleanup();
    materialTable_.destroy();
    materialRows_.reset();
    drawPass_.destroy();
    layout_.reset();
    vkCtx_->flushDeferredCleanup();
    geometry_.destroy();

    // Destroy pipelines
    destroy(device, opaquePipeline_);
    destroy(device, alphaTestPipeline_);
    destroy(device, transparentPipeline_);
    destroy(device, wireframePipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, materialDescPool_);
    destroy(device, materialSetLayout_);

    // Destroy shadow resources
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroy(device, shadowDescPool_);
    destroy(device, shadowSetLayout_);

    vkCtx_ = nullptr;
    initialized_ = false;
}

bool WMORenderer::loadModel(const pipeline::WMOModel& model, uint32_t id) {
    // No budget: run to completion, which is what callers outside terrain
    // streaming expect.
    for (;;) {
        const ModelLoadResult r = loadModelIncremental(model, id, 0.0f);
        if (r == ModelLoadResult::InProgress) continue;
        return r == ModelLoadResult::Complete;
    }
}

WMORenderer::ModelLoadResult WMORenderer::loadModelIncremental(
        const pipeline::WMOModel& model, uint32_t id, float budgetMs) {
    if (!model.isValid()) {
        core::Logger::getInstance().error("Cannot load invalid WMO model ", id);
        return ModelLoadResult::Failed;
    }
    // Prepared here, once, on the first call for the model - and not at all
    // for a model that is resident already, which is most calls.
    auto prepIt = preparedHere_.find(id);
    if (prepIt == preparedHere_.end()) {
        if (const auto admitted = admitModel(id, !model.textures.empty())) return *admitted;
        prepIt = preparedHere_.emplace(id, prepareModel(model)).first;
    }
    const std::shared_ptr<WmoModelPrep> prep = prepIt->second;
    const ModelLoadResult result = loadPreparedIncremental(*prep, id, budgetMs);
    if (result != ModelLoadResult::InProgress) preparedHere_.erase(id);
    return result;
}

std::optional<WMORenderer::ModelLoadResult> WMORenderer::admitModel(uint32_t id, bool hasTexturePaths) {
    // Check if already loaded
    auto existingIt = loadedModels.find(id);
    if (existingIt != loadedModels.end()) {
        // If a model was first loaded while texture resolution failed (or before
        // assets were fully available), it can remain permanently white because
        // merged batches cache texture pointers at load time. Do a one-time reload for
        // models that have texture paths but no resolved non-white textures.
        if (assetManager && hasTexturePaths) {
            bool hasResolvedTexture = false;
            for (const TextureRef& tex : existingIt->second.textures) {
                if (tex && tex != whiteTexture_) {
                    hasResolvedTexture = true;
                    break;
                }
            }
            // Track which WMO models have been force-reloaded after resolving only to
            // fallback textures. Cap the set to avoid unbounded memory growth in worlds
            // with many unique WMO groups (e.g. Dalaran has 2000+).
            static constexpr size_t kMaxRetryTracked = 8192;
            static std::unordered_set<uint32_t> retryReloadedModels;
            static bool retryReloadedModelsCapped = false;
            if (retryReloadedModels.size() > kMaxRetryTracked) {
                retryReloadedModels.clear();
                if (!retryReloadedModelsCapped) {
                    core::Logger::getInstance().warning("WMO fallback-retry set exceeded ", kMaxRetryTracked, " entries; reset");
                    retryReloadedModelsCapped = true;
                }
            }
            if (!hasResolvedTexture && retryReloadedModels.insert(id).second) {
                core::Logger::getInstance().warning(
                    "WMO model ", id,
                    " has only fallback textures; forcing one-time reload");
                unloadModel(id);
            } else {
                return ModelLoadResult::Complete;
            }
        } else {
            return ModelLoadResult::Complete;
        }
    }
    if (loadedModels.size() >= modelCacheLimit_) {
        if (modelLimitRejectWarnings_ < 3) {
            core::Logger::getInstance().warning("WMO model cache full (",
                                                loadedModels.size(), "/", modelCacheLimit_,
                                                "), skipping model load: id=", id);
        }
        ++modelLimitRejectWarnings_;
        return ModelLoadResult::Failed;
    }
    return std::nullopt;
}

WMORenderer::ModelLoadResult WMORenderer::loadPreparedIncremental(WmoModelPrep& prep, uint32_t id,
                                                                  float budgetMs) {
    // A load not yet begun may have nothing to do: the model may be resident
    // already, or the cache full.
    if (scene_->loadingModels_.find(id) == scene_->loadingModels_.end()) {
        if (const auto admitted = admitModel(id, !prep.texturePaths.empty())) return *admitted;
        if (prep.groups.empty()) {
            core::Logger::getInstance().warning("No valid groups loaded for WMO ", id,
                                                " - it has nothing to draw and is not placed");
            return ModelLoadResult::Failed;
        }
        core::Logger::getInstance().debug("Loading WMO model ", id, " with ", prep.groups.size(),
                                          " groups, ", prep.texturePaths.size(), " textures...");
    }

    // The accumulating model lives across calls, so a resumed load picks up its
    // textures, materials and the groups already uploaded.
    ModelData& modelData = scene_->loadingModels_[id];

    // Where a step of the load goes, in the frame profile under "WMO model
    // load": a model too large for one step spends it in one of these.
    struct LoadParts {
        CpuStageTimes& t;
        const char* current = "setup and textures";
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        void to(const char* next) {
            const auto now = std::chrono::steady_clock::now();
            t.add("WMO model load", current, std::chrono::duration<float, std::milli>(now - since).count());
            current = next; since = now;
        }
        ~LoadParts() { to(nullptr); }
    } loadParts{vkCtx_->gameCpuStages()};

    // Batch all GPU uploads (textures, VBs, IBs) into a single command buffer
    // submission with one fence wait, instead of one per upload.
    geometry_.releaseEmptyPages();
    vkCtx_->beginUploadBatch();

    // Textures and materials are model-level and done once; a resumed call has
    // them already and goes straight to the remaining groups.
    if (!modelData.setupDone) {
        // What the model says of itself, as prepared. Set again on a resumed
        // call that is still loading textures; it is the same each time.
        modelData.id = id;
        modelData.boundingBoxMin = prep.boundingBoxMin;
        modelData.boundingBoxMax = prep.boundingBoxMax;
        modelData.wmoAmbientColor = prep.ambientColor;
        modelData.isLowPlatform = prep.isLowPlatform;
        modelData.minimap = prep.minimap;
        modelData.textureNames = prep.textureNames;
        modelData.materialTextureIndices = prep.materialTextureIndices;
        modelData.materialBlendModes = prep.materialBlendModes;
        modelData.materialFlags = prep.materialFlags;
        modelData.materialSidnColors = prep.materialSidnColors;

        // Load textures for this model
        if (assetManager && !prep.texturePaths.empty()) {
            const auto texStart = std::chrono::steady_clock::now();
            for (size_t i = modelData.nextTextureIndex; i < prep.texturePaths.size(); i++) {
                if (budgetMs > 0.0f && i > modelData.nextTextureIndex) {
                    const float spent = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - texStart).count();
                    if (spent >= budgetMs) {
                        modelData.nextTextureIndex = i;
                        vkCtx_->endUploadBatch();
                        return ModelLoadResult::InProgress;  // resume at this texture
                    }
                }
                LoadedTexture loaded = loadTexture(prep.texturePaths[i]);
                modelData.textures.push_back(std::move(loaded.diffuse));
                modelData.normalMaps.push_back(std::move(loaded.normalHeight));
            }
        }
        modelData.nextTextureIndex = prep.texturePaths.size();
        modelData.setupDone = true;
    }

    // Upload each group and make its materials, a bounded number per call. A
    // model with hundreds of groups would otherwise upload them all in one
    // step: the worst measured was 286 groups at 131ms, against an 8ms budget.
    loadParts.to("groups");
    const auto groupStart = std::chrono::steady_clock::now();
    const size_t firstGroupThisCall = modelData.nextGroupIndex;
    for (size_t gi = modelData.nextGroupIndex; gi < prep.groups.size(); gi++) {
        if (budgetMs > 0.0f && gi > firstGroupThisCall) {
            const float spent = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - groupStart).count();
            if (spent >= budgetMs) {
                // Resume AT this group, not after it. Marking it done before
                // deciding whether to stop dropped one group on the floor at
                // every budget break, and a dropped group is a missing piece of
                // the building - the interior floors past a doorway in
                // Stormwind, on a model big enough to break several times.
                modelData.nextGroupIndex = gi;
                vkCtx_->endUploadBatch();
                return ModelLoadResult::InProgress;
            }
        }
        modelData.nextGroupIndex = gi + 1;
        WmoModelPrep::Group& prepared = prep.groups[gi];
        GroupResources resources = std::move(prepared.resources);

        // Unplaced, said by the pool, the group is not drawn.
        resources.geometry = geometry_.place(prepared.vertices.data(), static_cast<uint32_t>(prepared.vertices.size()),
                                             prepared.indices.data(), static_cast<uint32_t>(prepared.indices.size()));
        // In the staging copy now; the preparation need not hold them.
        prepared.vertices = {};
        prepared.indices = {};

        // Its merged batches, with the textures the model has now.
        const auto groupIndex = static_cast<uint32_t>(modelData.groups.size());
        resources.mergedBatches.reserve(prepared.merges.size());
        for (auto& merge : prepared.merges) {
            const WmoBatchState& state = merge.state;
            const uint32_t texIndex = state.texture;
            VkTexture* tex = whiteTexture_.get();
            uint32_t pendingTexture = UINT32_MAX;
            if (texIndex < modelData.textures.size()) {
                tex = modelData.textures[texIndex].get();
                if (!tex) tex = whiteTexture_.get();
                if (tex == whiteTexture_.get() && texIndex < modelData.textureNames.size() &&
                    !modelData.textureNames[texIndex].empty()) {
                    pendingTexture = texIndex;
                }
            }
            GroupResources::MergedBatch mb;
            mb.texture = tex;
            mb.textureHandle = tex == whiteTexture_.get() ? whiteTexture_.tableHandle()
                                                          : modelData.textures[texIndex].tableHandle();
            mb.pendingTextureIndex = pendingTexture;
            mb.hasTexture = tex != whiteTexture_.get();
            mb.alphaTest = state.blendMode == 1;
            mb.isTransparent = state.blendMode >= 2;
            mb.twoSided = state.twoSided;
            mb.sidnColor = state.sidnColor;
            mb.isLava = state.isLava;
            mb.emissiveLevel = state.emissiveLevel;
            mb.clothTop = state.clothTop;
            mb.clothDrop = state.clothDrop;
            resources.swayReach = std::max(resources.swayReach, clothSwayReach(mb.clothDrop));
            mb.clothCentreX = state.clothCentreX;
            mb.clothCentreY = state.clothCentreY;
            mb.lighting = state.lighting;
            mb.draws = std::move(merge.draws);
            if (mb.hasTexture) attachNormalHeightMap(modelData, texIndex, mb);
            createMergedBatchMaterial(modelData, resources, mb);
            if (mb.pendingTextureIndex != UINT32_MAX) {
                modelData.pendingTextureBatches.emplace_back(
                    groupIndex, static_cast<uint32_t>(resources.mergedBatches.size()));
            }
            resources.mergedBatches.push_back(std::move(mb));
        }
        modelData.groups.push_back(std::move(resources));
        modelData.loadedGroups++;
    }

    vkCtx_->endUploadBatch();
    loadParts.to("portals, doodads");

    // Portals and doodad placements, as prepared.
    modelData.portalVertices = std::move(prep.portalVertices);
    modelData.portals = std::move(prep.portals);
    modelData.portalRefs = std::move(prep.portalRefs);
    modelData.groupPortalRefs = std::move(prep.groupPortalRefs);
    modelData.doodadTemplates = std::move(prep.doodadTemplates);

    // Every group that draws must have arrived. Uploading them across several
    // calls under a time budget makes it possible to lose one at a resume
    // point without anything else noticing: the model still loads, still
    // renders, and is simply missing pieces of itself.
    if (modelData.groups.size() != prep.groups.size()) {
        core::Logger::getInstance().error(
            "WMO ", id, " uploaded ", modelData.groups.size(), " of ", prep.groups.size(),
            " groups - geometry is missing from this model");
    }

    // Read before the move: the log line below reports what was stored, and
    // modelData no longer owns it afterwards.
    const uint32_t loadedGroupCount = modelData.loadedGroups;
    loadParts.to("ray tracing and commit");
    registerRtModel(id, modelData, prep);
    if (!modelData.pendingTextureBatches.empty()) modelsWithPendingTextures_.push_back(id);
    {
        auto shape = std::make_shared<collision::WmoShape>();
        shape->modelId = id;
        shape->boundingBoxMin = modelData.boundingBoxMin;
        shape->boundingBoxMax = modelData.boundingBoxMax;
        shape->isLowPlatform = modelData.isLowPlatform;
        shape->ambientColor = modelData.wmoAmbientColor;
        shape->originalGroups = prep.collisionGroups;
        shape->path = prep.collisionDoodads.path;
        shape->doodads = prep.collisionDoodads.doodads;
        shape->doodadSets = prep.collisionDoodads.doodadSets;
        shape->groups.reserve(modelData.groups.size());
        for (const auto& group : modelData.groups) shape->groups.push_back(group.shape);
        modelData.shape = std::move(shape);
        if (collision_) collision_->movement().addWmoModel(modelData.shape);
    }
    loadedModels[id] = std::move(modelData);
    scene_->loadingModels_.erase(id);
    // Instances placed while it loaded had no groups to answer from.
    layout_.setModel(id, describeDraws(loadedModels[id]));
    scene_->slots.modelChanged(id);
    markContentChanged();
    retryPendingTextures();
    core::Logger::getInstance().debug("WMO model ", id, " loaded successfully (", loadedGroupCount, " groups)");
    return ModelLoadResult::Complete;
}

bool WMORenderer::isModelLoaded(uint32_t id) const {
    return loadedModels.find(id) != loadedModels.end();
}

void WMORenderer::unloadModel(uint32_t id) {
    auto it = loadedModels.find(id);
    if (it == loadedModels.end()) {
        return;
    }
    releaseRtModel(id);

    // Free GPU resources - defer because in-flight command buffers may
    // still reference this model's vertex/index buffers and descriptors.
    for (auto& group : it->second.groups) {
        destroyGroupGPU(group);
    }
    // Its groups, GPU handles handed to the deferred deletion above, are
    // collision geometry and grids now - the bulk of a city model, let go on
    // the background thread. Its textures go here: the cache they are shared
    // through is the main thread's.
    core::releaseInBackground(
        std::make_unique<std::vector<GroupResources>>(std::move(it->second.groups)));

    loadedModels.erase(it);
    layout_.removeModel(id);
    scene_->slots.modelChanged(id);
    markContentChanged();
    core::Logger::getInstance().info("WMO model ", id, " unloaded");
}

void WMORenderer::cleanupUnusedModels() {
    // Build set of model IDs that are still referenced by instances
    std::unordered_set<uint32_t> usedModelIds;
    for (const auto& instance : scene_->instances) {
        usedModelIds.insert(instance.modelId);
    }

    // Find and remove models with no instances
    std::vector<uint32_t> toRemove;
    for (const auto& [id, model] : loadedModels) {
        if (usedModelIds.find(id) == usedModelIds.end()) {
            toRemove.push_back(id);
        }
    }

    // unloadModel() routes every group buffer, material UBO, and descriptor
    // through deferAfterAllFrameFences(). Do not stall the entire device here;
    // periodic cleanup can otherwise introduce a visible hitch every time a
    // streamed WMO leaves the active set.
    for (uint32_t id : toRemove) {
        unloadModel(id);
    }

    if (!toRemove.empty()) {
        core::Logger::getInstance().info("WMO cleanup: removed ", toRemove.size(), " unused models, ", loadedModels.size(), " remaining");
    }
}

uint32_t WMORenderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                     const glm::vec3& rotation, float scale, uint32_t reservedId,
                                     uint16_t nameSet) {
    // Check if model is loaded
    if (loadedModels.find(modelId) == loadedModels.end()) {
        core::Logger::getInstance().error("Cannot create instance of unloaded WMO model ", modelId);
        return 0;
    }

    WMOInstance instance;
    instance.id = reservedId != 0 ? reservedId : nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;
    instance.nameSet = nameSet;
    instance.updateModelMatrix();
    const ModelData& model = loadedModels[modelId];
    transformAABB(instance.modelMatrix, model.boundingBoxMin, model.boundingBoxMax,
                  instance.worldBoundsMin, instance.worldBoundsMax);

    // Pre-compute world-space group bounds to avoid per-frame transformAABB
    instance.worldGroupBounds.reserve(model.groups.size());
    for (const auto& group : model.groups) {
        glm::vec3 gMin, gMax;
        transformAABB(instance.modelMatrix, group.boundingBoxMin, group.boundingBoxMax, gMin, gMax);
        gMin -= glm::vec3(0.5f);
        gMax += glm::vec3(0.5f);
        instance.worldGroupBounds.emplace_back(gMin, gMax);
    }

    scene_->instances.push_back(instance);
    size_t idx = scene_->instances.size() - 1;
    scene_->instanceIndexById[instance.id] = idx;
    scene_->slots.add(instance.id, modelId);
    if (collision_) collision_->placeWmo(instance.id, model.shape, instance.modelMatrix);
    markContentChanged();
    core::Logger::getInstance().debug("Created WMO instance ", instance.id, " (model ", modelId, ")");
    return instance.id;
}

std::optional<WmoMinimapPlacement> WMORenderer::minimapPlacement(uint32_t instanceId) const {
    const WMOInstance* instance = findInstance(instanceId);
    if (!instance) return std::nullopt;
    const auto model = loadedModels.find(instance->modelId);
    if (model == loadedModels.end() || !model->second.minimap) return std::nullopt;
    return WmoMinimapPlacement{.instance = instanceId, .model = model->second.minimap,
                               .modelToWorld = instance->modelMatrix, .worldToModel = instance->invModelMatrix,
                               .nameSet = instance->nameSet};
}

/// Recomputes an instance's world bounds from its model matrix.
///
/// Called whenever the matrix changes, which happens two ways: a plain move,
/// and a transport being handed a whole transform by the server. Both used to
/// do this themselves.
///
/// The half-unit of padding on each group's box is deliberate. The bounds are
/// what a collision query tests before it looks at triangles, and a box fitted
/// exactly to its geometry rejects a query that starts on the surface, so a
/// character standing on a floor can fail to find the floor it is standing on.
void WMORenderer::refreshInstanceBounds(WMOInstance& inst) {
    auto modelIt = loadedModels.find(inst.modelId);
    if (modelIt == loadedModels.end()) return;

    const ModelData& model = modelIt->second;
    transformAABB(inst.modelMatrix, model.boundingBoxMin, model.boundingBoxMax,
                  inst.worldBoundsMin, inst.worldBoundsMax);
    inst.worldGroupBounds.clear();
    inst.worldGroupBounds.reserve(model.groups.size());
    for (const auto& group : model.groups) {
        glm::vec3 gMin, gMax;
        transformAABB(inst.modelMatrix, group.boundingBoxMin, group.boundingBoxMax, gMin, gMax);
        gMin -= glm::vec3(0.5f);
        gMax += glm::vec3(0.5f);
        inst.worldGroupBounds.emplace_back(gMin, gMax);
    }
}

WMORenderer::WMOInstance* WMORenderer::findInstance(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    return idxIt == scene_->instanceIndexById.end() ? nullptr : &scene_->instances[idxIt->second];
}

const WMORenderer::WMOInstance* WMORenderer::findInstance(uint32_t instanceId) const {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    return idxIt == scene_->instanceIndexById.end() ? nullptr : &scene_->instances[idxIt->second];
}

void WMORenderer::setInstanceIsTransport(uint32_t instanceId, bool isTransport) {
    if (collision_) collision_->setWmoTransport(instanceId, isTransport);
}

void WMORenderer::setInstanceHidden(uint32_t instanceId, bool hidden) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    if (inst.hidden == hidden) return;
    inst.hidden = hidden;
    scene_->slots.changed(instanceId);
    if (collision_) collision_->setWmoHidden(instanceId, hidden);
    // Its doodads leave and come back with it. Hiding the hull alone left a
    // ship's sails, barrels and anchor hanging over the harbour while the
    // ship itself was on the other continent.
    if (m2Renderer_ && !inst.doodads.empty()) {
        std::vector<uint32_t> doodadIds;
        doodadIds.reserve(inst.doodads.size());
        for (const auto& doodad : inst.doodads) doodadIds.push_back(doodad.m2InstanceId);
        m2Renderer_->setInstancesHidden(doodadIds, hidden);
    }
}

void WMORenderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    // Where it already stands. The client sets a transport waiting at its dock
    // every frame; carried on to its doodads, that was some 770 M2s refiled,
    // re-read by the ray tracer and offered to the shadow cull again each
    // frame in Dalaran, for nothing that moved.
    if (inst.modelMatrix == transform) return;

    // Decompose transform to position/rotation/scale
    inst.position = glm::vec3(transform[3]);

    // Extract rotation (assuming uniform scale)
    glm::mat3 rotationMatrix(transform);
    float scaleX = glm::length(glm::vec3(transform[0]));
    float scaleY = glm::length(glm::vec3(transform[1]));
    float scaleZ = glm::length(glm::vec3(transform[2]));
    inst.scale = scaleX;  // Assume uniform scale

    if (scaleX > 0.0001f) rotationMatrix[0] /= scaleX;
    if (scaleY > 0.0001f) rotationMatrix[1] /= scaleY;
    if (scaleZ > 0.0001f) rotationMatrix[2] /= scaleZ;

    inst.rotation = glm::vec3(0.0f);  // Euler angles not directly used, so zero them

    // Update model matrix and bounds
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    refreshInstanceBounds(inst);
    scene_->slots.changed(instanceId);

    // Propagate transform to child M2 doodads (chairs, furniture on transports)
    if (m2Renderer_ && !inst.doodads.empty()) {
        for (const auto& doodad : inst.doodads) {
            glm::mat4 worldTransform = inst.modelMatrix * doodad.localTransform;
            m2Renderer_->setInstanceTransform(doodad.m2InstanceId, worldTransform);
        }
    }

    // A moving hull changes only its own placement. Rebuilding every building
    // here made each active ship pay for the entire streamed world once per
    // frame, even while the player was standing still in Dalaran.
    if (collision_) collision_->moveWmo(instanceId, inst.modelMatrix);
}

void WMORenderer::addDoodadToInstance(uint32_t instanceId, uint32_t m2InstanceId, const glm::mat4& localTransform) {
    WMOInstance* it = findInstance(instanceId);
    if (!it) {
        // Nothing to parent to. The M2 was already created, so silently dropping
        // it here leaves it stranded at the origin, drawn nowhere and owned by
        // no one - invisible in a way that looks exactly like a load failure.
        core::Logger::getInstance().warning(
            "WMO doodad has no parent instance ", instanceId,
            " - M2 instance ", m2InstanceId, " left at the origin");
        return;
    }
    WMOInstance::DoodadInfo doodad;
    doodad.m2InstanceId = m2InstanceId;
    doodad.localTransform = localTransform;
    it->doodads.push_back(doodad);

    // Place it immediately rather than waiting for the parent's next transform
    // push, so it is never drawn at the origin for a frame and never sits there
    // indefinitely if the parent is static.
    if (m2Renderer_) {
        m2Renderer_->setInstanceTransform(m2InstanceId, it->modelMatrix * localTransform);
        // A transport's doodads stream in over several frames, and the hull
        // may already be away on the other map by then.
        if (it->hidden) m2Renderer_->setInstanceHidden(m2InstanceId, true);
    }
}

size_t WMORenderer::setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId,
                                               bool loop) {
    if (!m2Renderer_) return 0;
    const WMOInstance* it = findInstance(instanceId);
    if (!it) return 0;
    for (const auto& doodad : it->doodads) {
        // A no-op for a doodad without that sequence, so the barrels and
        // lanterns sharing the deck are left alone.
        m2Renderer_->setInstanceAnimation(doodad.m2InstanceId, animationId, loop);
    }
    return it->doodads.size();
}

size_t WMORenderer::instanceDoodadCount(uint32_t instanceId) const {
    const WMOInstance* it = findInstance(instanceId);
    return it ? it->doodads.size() : 0;
}

const std::vector<WMORenderer::DoodadTemplate>* WMORenderer::getDoodadTemplates(uint32_t modelId) const {
    auto it = loadedModels.find(modelId);
    if (it != loadedModels.end() && !it->second.doodadTemplates.empty()) {
        return &it->second.doodadTemplates;
    }
    return nullptr;
}

bool WMORenderer::hasInstance(uint32_t instanceId) const {
    return findInstance(instanceId) != nullptr;
}

void WMORenderer::removeInstance(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt != scene_->instanceIndexById.end()) {
        auto it = scene_->instances.begin() + static_cast<std::ptrdiff_t>(idxIt->second);
        if (m2Renderer_) {
            std::vector<uint32_t> doodadIds;
            doodadIds.reserve(it->doodads.size());
            for (const auto& doodad : it->doodads) doodadIds.push_back(doodad.m2InstanceId);
            m2Renderer_->removeInstances(doodadIds);
        }
        scene_->rtChanges.release(scene_->slots.slotOf(instanceId), rtScene_);
        scene_->slots.remove(instanceId);
        scene_->instances.erase(it);
        rebuildIndexById();
        if (collision_) collision_->removeWmo(instanceId);
        markContentChanged();
        core::Logger::getInstance().debug("Removed WMO instance ", instanceId);
    }
}

void WMORenderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (instanceIds.empty() || scene_->instances.empty()) {
        return;
    }

    std::unordered_set<uint32_t> toRemove(instanceIds.begin(), instanceIds.end());
    if (m2Renderer_) {
        // In one batch: each single removal rescans every M2 instance in the
        // world to rebuild its index lists, and a town's WMOs carry hundreds
        // of doodads between them.
        std::vector<uint32_t> doodadIds;
        for (const auto& inst : scene_->instances) {
            if (toRemove.find(inst.id) == toRemove.end()) {
                continue;
            }
            for (const auto& doodad : inst.doodads) {
                doodadIds.push_back(doodad.m2InstanceId);
            }
        }
        m2Renderer_->removeInstances(doodadIds);
    }

    for (uint32_t id : toRemove) {
        scene_->rtChanges.release(scene_->slots.slotOf(id), rtScene_);
        scene_->slots.remove(id);
    }
    const size_t oldSize = scene_->instances.size();
    scene_->instances.erase(std::remove_if(scene_->instances.begin(), scene_->instances.end(),
                   [&toRemove](const WMOInstance& inst) {
                       return toRemove.find(inst.id) != toRemove.end();
                   }),
                   scene_->instances.end());

    if (scene_->instances.size() != oldSize) {
        rebuildIndexById();
        if (collision_) {
            for (uint32_t id : toRemove) collision_->removeWmo(id);
        }
        markContentChanged();
        core::Logger::getInstance().debug("Removed ", (oldSize - scene_->instances.size()),
                                          " WMO instances (batched)");
    }
}

void WMORenderer::clearInstances() {
    scene_->rtChanges.clear(rtScene_);
    scene_->slots.clear();
    if (m2Renderer_) {
        std::vector<uint32_t> doodadIds;
        for (const auto& inst : scene_->instances) {
            for (const auto& doodad : inst.doodads) doodadIds.push_back(doodad.m2InstanceId);
        }
        m2Renderer_->removeInstances(doodadIds);
    }
    // An unfinished model is map work, not a reusable cache entry. Its
    // already uploaded groups retire behind the same fences as full models.
    for (auto& [id, model] : scene_->loadingModels_) {
        releaseRtModel(id);
        for (auto& group : model.groups) destroyGroupGPU(group);
    }
    *scene_ = InstanceState{};
    layout_.clearInstances();
    if (collision_) collision_->clearWmo();
    // The preparations of those unfinished loads have handed over the
    // groups uploaded so far; a load begun again starts from a new one.
    preparedHere_.clear();
    markContentChanged();
    core::Logger::getInstance().info("Cleared all WMO instances");
}

void WMORenderer::clearAll() {
    clearInstances();

    if (vkCtx_) {

        // Free GPU resources for loaded models
        for (auto& [id, model] : loadedModels) {
            releaseRtModel(id);
            for (auto& group : model.groups) {
                destroyGroupGPU(group);
            }
        }

        // Deferred groups return individual descriptor sets; keep their pool.

    }

    // The models' textures go back to the shared cache with them, to be found
    // again by the next map or evicted when room is wanted.
    for (const auto& [id, model] : loadedModels) layout_.removeModel(id);
    loadedModels.clear();
    markContentChanged();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;

    LOG_INFO("Cleared all WMO models, instances, and texture cache");
}

void WMORenderer::rebuildIndexById() {
    scene_->instanceIndexById.clear();
    scene_->instanceIndexById.reserve(scene_->instances.size());
    for (size_t i = 0; i < scene_->instances.size(); i++) {
        scene_->instanceIndexById[scene_->instances[i].id] = i;
    }
}

void WMORenderer::prepareDraws(VkCommandBuffer cmd, uint32_t frameIndex, ::VkBuffer lights, uint32_t lightCount) {
    if (frameIndex >= 2) return;
    drawsReady_[frameIndex] = false;
    if (!recording_) return;
    const Frame& frame = *recording_;
    // The material rows first: the cull names them in its records.
    if (frame.materialRowCount == 0 || !materialTable_.reserve(cmd, frame.materialRowCount)) return;
    // Not acknowledged when it fails, so the same rows come with the next frame.
    if (!materialTable_.write(cmd, frameIndex, frame.materialUpdateSlots, frame.materialUpdateRows.data())) return;
    if (!drawPass_.prepare(cmd, frameIndex, frame.draws, lights, lightCount)) return;
    pendingSerial_ = frame.serial;

    const uint32_t drawVersion = drawPass_.drawVersion(frameIndex);
    if (materialSetVersion_[frameIndex] != materialTable_.version() ||
        materialSetDrawVersion_[frameIndex] != drawVersion) {
        const VkDescriptorBufferInfo infos[5] = {
            {.buffer = materialTable_.buffer(), .offset = 0, .range = materialTable_.bytes()},
            {.buffer = drawPass_.colourRecords(frameIndex), .offset = 0, .range = VK_WHOLE_SIZE},
            {.buffer = drawPass_.groupLights(frameIndex), .offset = 0, .range = VK_WHOLE_SIZE},
            {.buffer = drawPass_.groups().buffer(), .offset = 0, .range = drawPass_.groups().bytes()},
            {.buffer = drawPass_.instances().buffer(), .offset = 0, .range = drawPass_.instances().bytes()}};
        VkWriteDescriptorSet writes[5] = {};
        for (uint32_t b = 0; b < 5; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = materialSet_[frameIndex];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 5, writes, 0, nullptr);
        materialSetVersion_[frameIndex] = materialTable_.version();
        materialSetDrawVersion_[frameIndex] = drawVersion;
    }
    if (shadowSet_[frameIndex] && shadowSetVersion_[frameIndex] != drawVersion) {
        const VkDescriptorBufferInfo infos[3] = {
            {.buffer = drawPass_.casterRecords(frameIndex), .offset = 0, .range = VK_WHOLE_SIZE},
            {.buffer = drawPass_.groups().buffer(), .offset = 0, .range = drawPass_.groups().bytes()},
            {.buffer = drawPass_.instances().buffer(), .offset = 0, .range = drawPass_.instances().bytes()}};
        VkWriteDescriptorSet writes[3] = {};
        for (uint32_t b = 0; b < 3; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = shadowSet_[frameIndex];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 3, writes, 0, nullptr);
        shadowSetVersion_[frameIndex] = drawVersion;
    }
    drawsReady_[frameIndex] = true;
}

void WMORenderer::frameSubmitted() {
    if (pendingSerial_ == 0) return;
    frameApplied_.store(pendingSerial_, std::memory_order_release);
    pendingSerial_ = 0;
}

void WMORenderer::Frame::clear() noexcept {
    wireframe = false;
    serial = 0;
    materialRowCount = 0;
    materialUpdateSlots.clear();
    materialUpdateRows.clear();
    draws.clear();
}

std::vector<WmoGroupDraws> WMORenderer::describeDraws(const ModelData& model) const {
    constexpr uint32_t kAntiportal = 0x4000000;
    std::vector<WmoGroupDraws> out(model.groups.size());
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        const GroupResources& group = model.groups[gi];
        WmoGroupDraws& draws = out[gi];
        if (!group.geometry.placed()) continue;
        draws.page = group.geometry.page;
        draws.localCentre = (group.boundingBoxMin + group.boundingBoxMax) * 0.5f;
        draws.swayReach = group.swayReach;
        draws.shell = group.isLOD;
        draws.antiportal = (group.groupFlags & kAntiportal) != 0;
        for (const auto& mb : group.mergedBatches) {
            const uint32_t kind = wmoColourKind(mb.isTransparent, mb.alphaTest, mb.twoSided);
            const bool drawn = mb.materialRow != GpuRowStore<WmoMaterialGPU>::kNoRow;
            // An alpha-keyed material casts what it draws: Dalaran's lattice
            // windows, grates, fences. Until its texture is in, it casts
            // nothing rather than the solid quad.
            const bool casts = !(mb.alphaTest && !mb.hasTexture);
            for (const auto& dr : mb.draws) {
                if (dr.indexCount == 0) continue;
                WmoRangeGPU range;
                range.indexCount = dr.indexCount;
                range.firstIndex = group.geometry.firstIndex + dr.firstIndex;
                range.baseVertex = group.geometry.baseVertex;
                range.material = drawn ? mb.materialRow : WmoRangeGPU::kNone;
                range.place = kind | (draws.kindCounts[kind]++ << 3);
                if (casts) {
                    range.caster = draws.casterCount++;
                    range.texture = drawn ? materialRows_.at(mb.materialRow).texture : whiteTexture_.tableHandle();
                    range.casterFlags = mb.alphaTest ? 3u : 0u;
                }
                draws.ranges.push_back(range);
            }
        }
    }
    return out;
}

void WMORenderer::catchUpLayout() {
    scene_->slots.catchUp(InstanceSlots::Mirror::GpuCull, [&](uint32_t slot, uint32_t id) {
        if (id == InstanceSlots::invalid) {
            layout_.free(slot);
            return;
        }
        const auto index = scene_->instanceIndexById.find(id);
        if (index == scene_->instanceIndexById.end()) return;
        WMOInstance& instance = scene_->instances[index->second];
        const auto model = loadedModels.find(instance.modelId);
        // Placed before its model finished loading: its groups' boxes come
        // with the model.
        if (model != loadedModels.end() && instance.worldGroupBounds.size() != model->second.groups.size())
            refreshInstanceBounds(instance);
        layout_.place(slot, WmoDrawLayout::Placement{.model = instance.modelId,
                                                     .matrix = instance.modelMatrix,
                                                     .scale = instance.scale,
                                                     .shown = !instance.hidden,
                                                     .groupBounds = instance.worldGroupBounds});
    });
}

void WMORenderer::walkPortals(const Camera& camera, const glm::vec3& viewer, std::vector<uint32_t>& culled) const {
    culled.assign((layout_.groupRowCount() + 31) / 32, 0u);
    Frustum frustum;
    frustum.extractFromMatrix(camera.getProjectionMatrix() * camera.getViewMatrix());
    const glm::vec3 camPos = camera.getPosition();
    std::unordered_set<uint32_t> visible;
    for (const WMOInstance& instance : scene_->instances) {
        if (instance.hidden) continue;
        const auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt == loadedModels.end()) continue;
        const ModelData& model = modelIt->second;
        if (model.portals.empty() || model.portalRefs.empty()) continue;
        const std::span<const uint32_t> rows = layout_.groupRows(scene_->slots.slotOf(instance.id));
        if (rows.empty()) continue;

        // If the camera is outside all groups, skip portal culling entirely.
        // This is what makes it safe for the traversal below to start from
        // the camera: an orbiting third-person camera that has left the
        // building never reaches the walk at all.
        const glm::vec3 localRealCam = glm::vec3(instance.invModelMatrix * glm::vec4(camPos, 1.0f));
        const int camGroup = findContainingGroup(model, localRealCam);
        if (camGroup < 0) continue;
        // Entranceways and awnings: the best-fit AABB often claims an
        // interior group while the camera is visually outside (interior
        // boxes spill past the doorway). Only trust portal traversal when
        // the camera group is interior-only - the same rule
        // getVisibleGroupsViaPortals applies to the viewer position.
        constexpr uint32_t WMO_GROUP_FLAG_OUTDOOR = 0x8;
        constexpr uint32_t WMO_GROUP_FLAG_INDOOR = 0x2000;
        const uint32_t gFlags = model.groups[camGroup].groupFlags;
        if ((gFlags & WMO_GROUP_FLAG_INDOOR) == 0 || (gFlags & WMO_GROUP_FLAG_OUTDOOR) != 0) continue;
        // Doorway thresholds sit inside both the interior box and an outdoor
        // street group's box - treat those as outdoors too (second half of
        // the viewer-side rule).
        bool atThreshold = false;
        for (size_t gi = 0; gi < model.groups.size() && !atThreshold; ++gi) {
            if (static_cast<int>(gi) == camGroup) continue;
            const auto& g = model.groups[gi];
            if (!(g.groupFlags & WMO_GROUP_FLAG_OUTDOOR)) continue;
            atThreshold = localRealCam.x >= g.boundingBoxMin.x && localRealCam.x <= g.boundingBoxMax.x &&
                          localRealCam.y >= g.boundingBoxMin.y && localRealCam.y <= g.boundingBoxMax.y &&
                          localRealCam.z >= g.boundingBoxMin.z && localRealCam.z <= g.boundingBoxMax.z;
        }
        if (atThreshold) continue;

        // Both viewpoints seed the walk. The frustum belongs to the camera,
        // so its group is the principled start - but at a doorway or in a
        // hallway the camera and the character stand in different groups,
        // and seeding from one while testing doors against the other's view
        // is how the interior emptied out. Neither is reliably right on its
        // own, so take both.
        //
        // Ironforge shows this worst because of what rescues everywhere
        // else: exterior groups are seeded unconditionally below, which in a
        // city WMO means the streets and facades stay drawn however the walk
        // goes. Ironforge is interior throughout, that seed contributes
        // almost nothing, and the entire result rests on the traversal.
        const glm::vec3 localViewer = glm::vec3(instance.invModelMatrix * glm::vec4(viewer, 1.0f));
        visible.clear();
        getVisibleGroupsViaPortals(model, localRealCam, localViewer, frustum, instance.modelMatrix, visible);
        for (size_t gi = 0; gi < rows.size(); ++gi) {
            const uint32_t row = rows[gi];
            if (row == GpuRowStore<WmoGroupGPU>::kNoRow || visible.contains(static_cast<uint32_t>(gi))) continue;
            culled[row >> 5] |= 1u << (row & 31u);
        }
    }
}

std::shared_ptr<const WMORenderer::Frame> WMORenderer::extract(const Camera& camera, const glm::vec3& viewer,
                                                               const WaterReflectionView* mirror) {
    auto frame = framePool_.acquire();
    frame->wireframe = wireframeMode;
    // The settings are in every row; changed, every row is written again.
    if (std::exchange(materialSettingsDirty_, false)) rewriteMaterialRows();
    catchUpLayout();

    // The rows ride from the first frame extracted after they were written
    // until one that carried them is on the GPU.
    frame->serial = ++frameSerial_;
    const uint32_t applied = frameApplied_.load(std::memory_order_acquire);
    materialRows_.carry(frame->serial, applied, frame->materialUpdateSlots, frame->materialUpdateRows);
    frame->materialRowCount = materialRows_.count();
    WmoDrawFrame& draws = frame->draws;
    layout_.carry(frame->serial, applied, draws);
    draws.pages.resize(draws.cull.shape.x);
    for (size_t page = 0; page < draws.pages.size(); ++page)
        draws.pages[page] = {.vertices = geometry_.vertices(static_cast<int32_t>(page)),
                             .indices = geometry_.indices(static_cast<int32_t>(page))};

    // The cull's view: the camera's volume, the mirror's, the distances.
    WmoCullFrameGPU& cull = draws.cull;
    Frustum frustum;
    frustum.extractFromMatrix(camera.getProjectionMatrix() * camera.getViewMatrix());
    const auto planeOf = [](const Plane& p) { return glm::vec4(p.normal, p.distance); };
    for (int i = 0; i < 6; ++i) cull.viewPlanes[i] = planeOf(frustum.getPlane(static_cast<Frustum::Side>(i)));
    if (mirror) {
        for (int i = 0; i < 6; ++i) cull.mirrorPlanes[i] = planeOf(mirror->seen.getPlane(static_cast<Frustum::Side>(i)));
    }
    cull.camera = glm::vec4(camera.getPosition(), viewDistance_);
    // The nearer group distance and the portal walk run only when asked for
    // (WOWEE_WMO_CULL): they have removed nearby interiors before.
    const float groupDistance = distanceCulling ? std::min(viewDistance_, maxGroupDistance) : viewDistance_;
    cull.limits = glm::vec4(cullingEnabled_ ? groupDistance : -1.0f, mirror ? mirror->height : 0.0f,
                            mirror ? 1.0f : 0.0f, 0.0f);
    const bool walk = portalCulling && cullingEnabled_;
    cull.shape.y = walk ? 1u : 0u;
    if (walk) walkPortals(camera, viewer, draws.portalCulled);
    return frame;
}

void WMORenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, Pass pass, Layer layer) {
    if (!opaquePipeline_ || !recording_) return;
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if (!drawsReady_[frameIndex] || !drawPass_.ready(frameIndex)) return;
    const WmoDrawFrame& draws = recording_->draws;
    const auto pages = static_cast<uint32_t>(draws.pages.size());
    const ::VkBuffer commands = drawPass_.commands(frameIndex);

    // The per-frame set (0), the device's texture table (1), and the
    // material table with what the draws read (2).
    const VkDescriptorSet sets[3] = {perFrameSet, vkCtx_->textureTable().set(), materialSet_[frameIndex]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 3, sets, 0, nullptr);
    FaceCulling faces(*vkCtx_, cmd);
    const VkPipeline drawnWith = recording_->wireframe && wireframePipeline_ ? wireframePipeline_ : opaquePipeline_;

    // The layer's kinds, opaque ones first, each a call a page: the view's
    // list first, the reflection's after it.
    const uint32_t firstKind = layer == Layer::Blended ? kWmoBlendedKinds : 0u;
    const uint32_t endKind = layer == Layer::Opaque ? kWmoBlendedKinds : kWmoColourKinds;
    const uint32_t listBase = pass == Pass::Reflection ? draws.colourPlaces : 0u;
    for (uint32_t kind = firstKind; kind < endKind; ++kind) {
        // 0 opaque, 2 opaque with the alpha test compiled in (in wireframe,
        // the wireframe pipeline), 4 blended without depth writes.
        const VkPipeline pipeline = kind >= kWmoBlendedKinds ? transparentPipeline_
                                    : kind >= 2 && !recording_->wireframe ? alphaTestPipeline_
                                                                          : drawnWith;
        bool bound = false;
        for (uint32_t page = 0; page < pages; ++page) {
            const uint32_t bucket = page * kWmoColourKinds + kind;
            const uint32_t size = draws.bucketSizes[bucket];
            if (size == 0 || !draws.pages[page].vertices || !pipeline) continue;
            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                faces.pipelineBound();
                faces.show((kind & 1u) != 0u);
                bound = true;
            }
            geometry_.bind(cmd, GeometryPool::Range{.page = static_cast<int32_t>(page),
                                                    .vertices = draws.pages[page].vertices,
                                                    .indices = draws.pages[page].indices});
            for (uint32_t first = 0; first < size; first += maxDrawsPerCall_) {
                const WMOPushConstants push{.drawBase = draws.bucketStarts[bucket] + first};
                vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                vkCmdDrawIndexedIndirect(cmd, commands,
                                         VkDeviceSize(listBase + push.drawBase) * sizeof(VkDrawIndexedIndirectCommand),
                                         std::min(maxDrawsPerCall_, size - first),
                                         sizeof(VkDrawIndexedIndirectCommand));
            }
        }
    }
}

bool WMORenderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // Set 1: the frame slot's caster records, the group and instance tables.
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings(3);
        for (uint32_t b = 0; b < 3; ++b)
            bindings[b] = {.binding = b, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                           .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
        shadowSetLayout_ = createDescriptorSetLayout(device, bindings);
        const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 6};
        VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        const VkDescriptorSetLayout layouts[2] = {shadowSetLayout_, shadowSetLayout_};
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        if (!shadowSetLayout_ || vkCreateDescriptorPool(device, &poolInfo, nullptr, &shadowDescPool_) != VK_SUCCESS ||
            (ai.descriptorPool = shadowDescPool_, vkCtx_->allocateDescriptorSets(ai, shadowSet_)) != VK_SUCCESS) {
            LOG_ERROR("WMORenderer: could not make the shadow casters' sets; buildings cast no shadow");
            return false;
        }
    }

    // Set 0: the texture table the records name alpha-keyed textures in.
    const VkPushConstantRange pc{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(WmoShadowPush)};
    shadowPipelineLayout_ = createPipelineLayout(device, {vkCtx_->textureTable().layout(), shadowSetLayout_}, {pc});
    if (!shadowPipelineLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create shadow pipeline layout");
        return false;
    }

    // The doodads' fragment shader: a batch's alpha-keyed texture cuts its
    // shadow by the record's handle.
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/wmo_shadow.vert.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/m2_shadow.frag.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load shadow fragment shader");
        return false;
    }

    const VkVertexInputBindingDescription vertBind = perVertexBinding(sizeof(WMOVertex));
    const std::vector<VkVertexInputAttributeDescription> vertAttrs =
        toVkAttributes(kWmoShadowVertexAttributes);

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        core::Logger::getInstance().error("WMORenderer: failed to create shadow pipeline");
        return false;
    }
    core::Logger::getInstance().info("WMORenderer shadow pipeline initialized");
    return true;
}

std::optional<uint32_t> WMORenderer::cullShadowMap(VkCommandBuffer cmd, const glm::vec3& centre, float radius) {
    if (!shadowPipeline_) return std::nullopt;
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if (!drawsReady_[frameIndex]) return std::nullopt;
    // The map's square, at least: distant buildings whose shadows reach the
    // player are cast into it too.
    return drawPass_.cullShadowMap(cmd, frameIndex, centre, std::max(radius, 180.0f));
}

void WMORenderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, uint32_t map) {
    if (!shadowPipeline_ || !recording_) return;
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if (!drawsReady_[frameIndex]) return;
    const WmoDrawFrame& draws = recording_->draws;
    const auto pages = static_cast<uint32_t>(draws.pages.size());
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    const VkDescriptorSet sets[2] = {vkCtx_->textureTable().set(), shadowSet_[frameIndex]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_, 0, 2, sets, 0, nullptr);
    const uint32_t mapBase = 2 * draws.colourPlaces + map * draws.casterPlaces;
    for (uint32_t page = 0; page < pages; ++page) {
        const uint32_t bucket = pages * kWmoColourKinds + page;
        const uint32_t size = draws.bucketSizes[bucket];
        if (size == 0 || !draws.pages[page].vertices) continue;
        geometry_.bind(cmd, GeometryPool::Range{.page = static_cast<int32_t>(page),
                                                .vertices = draws.pages[page].vertices,
                                                .indices = draws.pages[page].indices});
        for (uint32_t first = 0; first < size; first += maxDrawsPerCall_) {
            const WmoShadowPush push{.lightSpace = lightSpaceMatrix,
                                     .call = glm::uvec4(draws.bucketStarts[bucket] + first, 0u, 0u, 0u)};
            vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            vkCmdDrawIndexedIndirect(cmd, drawPass_.commands(frameIndex),
                                     VkDeviceSize(mapBase + push.call.x) * sizeof(VkDrawIndexedIndirectCommand),
                                     std::min(maxDrawsPerCall_, size - first), sizeof(VkDrawIndexedIndirectCommand));
        }
    }
}

uint32_t WMORenderer::getTotalTriangleCount() const {
    uint32_t total = 0;
    for (const auto& instance : scene_->instances) {
        auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt != loadedModels.end()) {
            total += modelIt->second.getTotalTriangles();
        }
    }
    return total;
}

// renderGroup removed - draw calls are inlined in render()

void WMORenderer::destroyGroupGPU(GroupResources& group) {
    if (!vkCtx_) return;
    // Its ranges come back after the frames in flight; a frame still being
    // recorded holds the group's draw data, range and all.
    geometry_.release(group.geometry);
    group.geometry = {};
    // And its material rows, after the frames that may draw with them.
    for (auto& mb : group.mergedBatches)
        materialRows_.release(*vkCtx_, std::exchange(mb.materialRow, GpuRowStore<WmoMaterialGPU>::kNoRow));
}

void WMORenderer::attachNormalHeightMap(const ModelData& model, uint32_t texIndex,
                                        GroupResources::MergedBatch& mb) const {
    if (!mb.texture || mb.texture == whiteTexture_.get()) return;
    if (texIndex >= model.normalMaps.size() || !model.normalMaps[texIndex]) return;
    const TextureRef& normals = model.normalMaps[texIndex];
    mb.normalHeightMap = normals.get();
    mb.normalHandle = normals.tableHandle();
    mb.heightMapVariance = normals.info().heightVariance;
}

void WMORenderer::createMergedBatchMaterial(const ModelData& model, const GroupResources& group,
                                            GroupResources::MergedBatch& mb) {
    // The baseline probe reproduces the lighting from before a portal-interior
    // group could be sunlit at all: every batch of one baked, and unshadowed.
    const bool portalInterior = (group.groupFlags & 0x2000) != 0;
    static const bool baselineProbe = std::getenv("WOWEE_ORGRIMMAR_PROBE") &&
                                      std::getenv("WOWEE_ORGRIMMAR_SHADOW_BASELINE");
    const WmoBatchLighting lighting = baselineProbe && portalInterior
        ? WmoBatchLighting{ .light = WmoLight::VertexColour }
        : mb.lighting;

    // Its textures' places, its own where they are valid and the fallbacks
    // where not - not merely non-null: a texture whose upload or view failed
    // sampled through a live descriptor is undefined behaviour that reaches
    // an NVIDIA driver as a lost device (#123). A batch even whose fallback
    // has no place is not drawn: an unlit wall costs a wall, a null view the
    // device.
    const auto handle = [](const VkTexture* wanted, TextureHandle wantedHandle, const TextureRef& fallback) {
        return wanted && wanted->isValid() && wantedHandle != kNoTextureHandle ? wantedHandle
                                                                               : fallback.tableHandle();
    };
    WmoMaterialGPU matData{};
    matData.texture = handle(mb.texture, mb.textureHandle, whiteTexture_);
    matData.normalHeight = handle(mb.normalHeightMap, mb.normalHandle, flatNormalTexture_);
    if (matData.texture == kNoTextureHandle || matData.normalHeight == kNoTextureHandle) {
        static core::LogBudget budget(8, "WMO batches with no texture place");
        if (budget.take())
            LOG_WARNING("WMO ", model.id, ": a batch's texture has no place in the texture table - the batch is "
                        "not drawn");
        materialRows_.release(*vkCtx_, std::exchange(mb.materialRow, GpuRowStore<WmoMaterialGPU>::kNoRow));
        return;
    }
    matData.hasTexture = mb.hasTexture ? 1 : 0;
    matData.alphaTest = mb.alphaTest ? 1 : 0;
    matData.light = static_cast<int32_t>(lighting.light);
    matData.transitionLight = lighting.transition ? static_cast<int32_t>(lighting.under) : -1;
    matData.receivesExteriorShadows = !baselineProbe &&
        wmoReceivesExteriorShadows(group.groupFlags) ? 1 : 0;
    matData.hasVertexColors = (group.groupFlags & pipeline::kWMOGroupHasVertexColors) != 0 ? 1 : 0;
    matData.specularIntensity = 0.5f;
    // MOMT colours are BGRA bytes.
    matData.sidn = glm::vec4(static_cast<float>((mb.sidnColor >> 16) & 0xffu) / 255.0f,
                             static_cast<float>((mb.sidnColor >> 8) & 0xffu) / 255.0f,
                             static_cast<float>(mb.sidnColor & 0xffu) / 255.0f, 0.0f);
    matData.enableNormalMap = normalMappingEnabled_ ? 1 : 0;
    matData.enablePOM = pomEnabled_ ? 1 : 0;
    matData.pomScale = 0.012f;
    matData.pomMaxSamples = pomSamplesFor(pomQuality_);
    matData.heightMapVariance = mb.heightMapVariance;
    matData.normalMapStrength = normalMapStrength_;
    matData.isLava = mb.isLava ? 1 : 0;
    matData.ambient = glm::vec4(model.wmoAmbientColor, 0.0f);
    matData.emissive = static_cast<int32_t>(mb.emissiveLevel);
    matData.cloth = glm::vec4(mb.clothTop, mb.clothDrop, mb.clothCentreX, mb.clothCentreY);
    if (mb.materialRow == GpuRowStore<WmoMaterialGPU>::kNoRow) mb.materialRow = materialRows_.place();
    materialRows_.write(mb.materialRow, matData);
}

void WMORenderer::rewriteMaterialRows() {
    for (auto* models : {&loadedModels, &scene_->loadingModels_}) {
        for (auto& [id, model] : *models) {
            for (auto& group : model.groups)
                for (auto& mb : group.mergedBatches) createMergedBatchMaterial(model, group, mb);
        }
    }
    // A batch whose row came or went draws from it, or stops, from now on.
    for (const auto& [id, model] : loadedModels)
        if (layout_.setModel(id, describeDraws(model))) scene_->slots.modelChanged(id);
}

void WMORenderer::retryPendingTextures() {
    for (size_t m = 0; m < modelsWithPendingTextures_.size();) {
        auto it = loadedModels.find(modelsWithPendingTextures_[m]);
        if (it == loadedModels.end()) {
            modelsWithPendingTextures_[m] = modelsWithPendingTextures_.back();
            modelsWithPendingTextures_.pop_back();
            continue;
        }
        ModelData& model = it->second;
        auto& pending = model.pendingTextureBatches;
        const size_t pendingBefore = pending.size();
        for (size_t p = 0; p < pending.size();) {
            const auto [groupIndex, batchIndex] = pending[p];
            if (groupIndex >= model.groups.size() ||
                batchIndex >= model.groups[groupIndex].mergedBatches.size()) {
                pending[p] = pending.back();
                pending.pop_back();
                continue;
            }
            GroupResources& group = model.groups[groupIndex];
            auto& mb = group.mergedBatches[batchIndex];
            const uint32_t texIndex = mb.pendingTextureIndex;
            if (texIndex >= model.textureNames.size() || texIndex >= model.textures.size()) {
                pending[p] = pending.back();
                pending.pop_back();
                continue;
            }
            // loadTexture decides when a failed path is worth reading again;
            // asking before then is a lookup that hands back white.
            LoadedTexture loaded = loadTexture(model.textureNames[texIndex]);
            if (!loaded.diffuse || loaded.diffuse == whiteTexture_) {
                ++p;
                continue;
            }

            // Its row is rewritten: the frames in flight draw with the row as
            // they carried it.
            mb.texture = loaded.diffuse.get();
            mb.textureHandle = loaded.diffuse.tableHandle();
            model.textures[texIndex] = std::move(loaded.diffuse);
            if (texIndex < model.normalMaps.size()) model.normalMaps[texIndex] = std::move(loaded.normalHeight);
            mb.hasTexture = true;
            mb.pendingTextureIndex = UINT32_MAX;
            attachNormalHeightMap(model, texIndex, mb);
            createMergedBatchMaterial(model, group, mb);
            group.allUntextured = false;
            core::Logger::getInstance().info("WMO ", model.id, " got texture '",
                                             model.textureNames[texIndex], "' on a retry");
            pending[p] = pending.back();
            pending.pop_back();
        }
        // The batches given their texture draw and cast with it.
        if (pending.size() != pendingBefore && layout_.setModel(model.id, describeDraws(model)))
            scene_->slots.modelChanged(model.id);
        if (pending.empty()) {
            modelsWithPendingTextures_[m] = modelsWithPendingTextures_.back();
            modelsWithPendingTextures_.pop_back();
        } else {
            ++m;
        }
    }
}


bool WMORenderer::isGroupVisible(const GroupResources& group, const glm::mat4& modelMatrix,
                                 const Camera& camera) const {
    // Proper frustum-AABB intersection test for accurate visibility culling
    // Transform bounding box min/max to world space
    glm::vec3 localMin = group.boundingBoxMin;
    glm::vec3 localMax = group.boundingBoxMax;

    // Transform min and max to world space
    glm::vec4 worldMinH = modelMatrix * glm::vec4(localMin, 1.0f);
    glm::vec4 worldMaxH = modelMatrix * glm::vec4(localMax, 1.0f);
    glm::vec3 worldMin = glm::vec3(worldMinH);
    glm::vec3 worldMax = glm::vec3(worldMaxH);

    // Ensure min/max are correct after transformation (handles non-uniform scaling)
    glm::vec3 boundsMin = glm::min(worldMin, worldMax);
    glm::vec3 boundsMax = glm::max(worldMin, worldMax);

    // Extract frustum planes from view-projection matrix
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());

    // Test if AABB intersects view frustum
    return frustum.intersectsAABB(boundsMin, boundsMax);
}

int WMORenderer::findContainingGroup(const ModelData& model, const glm::vec3& localPos) const {
    // Find which group's bounding box contains the position
    // Prefer interior groups (smaller volume) when multiple match
    int bestGroup = -1;
    float bestVolume = std::numeric_limits<float>::max();

    for (size_t gi = 0; gi < model.groups.size(); gi++) {
        const auto& group = model.groups[gi];
        if (localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
            localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
            localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
            glm::vec3 size = group.boundingBoxMax - group.boundingBoxMin;
            float volume = size.x * size.y * size.z;
            if (volume < bestVolume) {
                bestVolume = volume;
                bestGroup = static_cast<int>(gi);
            }
        }
    }
    return bestGroup;
}

bool WMORenderer::isPortalVisible(const ModelData& model, uint16_t portalIndex,
                                   [[maybe_unused]] const glm::vec3& cameraLocalPos,
                                   const Frustum& frustum,
                                   const glm::mat4& modelMatrix) const {
    if (portalIndex >= model.portals.size()) return false;

    const auto& portal = model.portals[portalIndex];
    if (portal.vertexCount < 3) return false;
    if (portal.startVertex + portal.vertexCount > model.portalVertices.size()) return false;

    // Get portal polygon center and bounds for frustum test
    glm::vec3 center(0.0f);
    glm::vec3 pMin = model.portalVertices[portal.startVertex];
    glm::vec3 pMax = pMin;
    for (uint16_t i = 0; i < portal.vertexCount; i++) {
        const auto& v = model.portalVertices[portal.startVertex + i];
        center += v;
        pMin = glm::min(pMin, v);
        pMax = glm::max(pMax, v);
    }
    center /= static_cast<float>(portal.vertexCount);

    // Transform all 8 corners to world space to build the correct world AABB.
    // Direct transform of pMin/pMax is wrong for rotated WMOs - the matrix can
    // swap or negate components, inverting min/max and causing frustum test failures.
    const glm::vec3 corners[8] = {
        {pMin.x, pMin.y, pMin.z}, {pMax.x, pMin.y, pMin.z},
        {pMin.x, pMax.y, pMin.z}, {pMax.x, pMax.y, pMin.z},
        {pMin.x, pMin.y, pMax.z}, {pMax.x, pMin.y, pMax.z},
        {pMin.x, pMax.y, pMax.z}, {pMax.x, pMax.y, pMax.z},
    };
    glm::vec3 worldMin( std::numeric_limits<float>::max());
    glm::vec3 worldMax(-std::numeric_limits<float>::max());
    for (const auto& c : corners) {
        glm::vec3 wc = glm::vec3(modelMatrix * glm::vec4(c, 1.0f));
        worldMin = glm::min(worldMin, wc);
        worldMax = glm::max(worldMax, wc);
    }

    // Check if portal AABB intersects frustum (more robust than point test)
    return frustum.intersectsAABB(worldMin, worldMax);
}

void WMORenderer::getVisibleGroupsViaPortals(const ModelData& model,
                                              const glm::vec3& cameraLocalPos,
                                              const glm::vec3& viewerLocalPos,
                                              const Frustum& frustum,
                                              const glm::mat4& modelMatrix,
                                              std::unordered_set<uint32_t>& outVisibleGroups) const {
    constexpr uint32_t WMO_GROUP_FLAG_OUTDOOR = 0x8;
    constexpr uint32_t WMO_GROUP_FLAG_INDOOR = 0x2000;

    // Find camera's containing group
    int cameraGroup = findContainingGroup(model, cameraLocalPos);

    // If camera is outside all groups, fall back to frustum culling only
    if (cameraGroup < 0) {
        // Camera outside WMO - mark all groups as potentially visible
        // (will still be frustum culled in render)
        for (size_t gi = 0; gi < model.groups.size(); gi++) {
            outVisibleGroups.insert(static_cast<uint32_t>(gi));
        }
        return;
    }

    // Outdoor city WMOs (e.g. Stormwind) often have portal graphs that are valid for
    // indoor visibility but too aggressive outdoors, causing direction-dependent popout.
    // Only trust portal traversal when the camera is in an interior-only group.
    if (cameraGroup < static_cast<int>(model.groups.size())) {
        const uint32_t gFlags = model.groups[cameraGroup].groupFlags;
        const bool isIndoor = (gFlags & WMO_GROUP_FLAG_INDOOR) != 0;
        const bool isOutdoor = (gFlags & WMO_GROUP_FLAG_OUTDOOR) != 0;
        if (!isIndoor || isOutdoor) {
            for (size_t gi = 0; gi < model.groups.size(); gi++) {
                outVisibleGroups.insert(static_cast<uint32_t>(gi));
            }
            return;
        }
        // Best-fit group is indoor-only, but the position might also be inside an
        // outdoor group's AABB (e.g., standing on a street near a building whose
        // indoor AABB extends outward).  If any outdoor group also contains the
        // position, treat this as an outdoor location and show all groups.
        for (size_t gi = 0; gi < model.groups.size(); gi++) {
            if (static_cast<int>(gi) == cameraGroup) continue;
            const auto& g = model.groups[gi];
            if (!(g.groupFlags & WMO_GROUP_FLAG_OUTDOOR)) continue;
            if (cameraLocalPos.x >= g.boundingBoxMin.x && cameraLocalPos.x <= g.boundingBoxMax.x &&
                cameraLocalPos.y >= g.boundingBoxMin.y && cameraLocalPos.y <= g.boundingBoxMax.y &&
                cameraLocalPos.z >= g.boundingBoxMin.z && cameraLocalPos.z <= g.boundingBoxMax.z) {
                for (size_t gj = 0; gj < model.groups.size(); gj++) {
                    outVisibleGroups.insert(static_cast<uint32_t>(gj));
                }
                return;
            }
        }
    }

    // If the camera group has no portal refs, it's a dead-end group (utility/transition group).
    // Fall back to showing all groups to avoid the rest of the WMO going invisible.
    if (cameraGroup < static_cast<int>(model.groupPortalRefs.size())) {
        auto [portalStart, portalCount] = model.groupPortalRefs[cameraGroup];
        if (portalCount == 0) {
            for (size_t gi = 0; gi < model.groups.size(); gi++) {
                outVisibleGroups.insert(static_cast<uint32_t>(gi));
            }
            return;
        }
    }

    // Exterior groups are never portal-culled. AABB containment misclassifies
    // doorway/awning thresholds as indoors (interior boxes spill past the
    // door and outdoor boxes don't always overlap them), and a wrong BFS
    // start then hides the whole city. Portal traversal below only decides
    // interior-only groups; streets and facades always draw (distance culling
    // still bounds them).
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        const uint32_t f = model.groups[gi].groupFlags;
        if (!(f & WMO_GROUP_FLAG_INDOOR) || (f & WMO_GROUP_FLAG_OUTDOOR))
            outVisibleGroups.insert(static_cast<uint32_t>(gi));
    }

    // BFS through portals from both viewpoints' groups plus every always-visible
    // exterior group, so interiors seen through open doors still draw even when
    // one viewpoint's containing group was misclassified.
    //
    // The character's group is seeded alongside the camera's because in a
    // doorway or a hallway the two are in different rooms. Walking from only one
    // of them, while judging every door against the camera's frustum, is what
    // emptied Ironforge. A second seed can only add groups, never remove any,
    // and drawing a room too many is the failure this should have.
    std::vector<bool> visited(model.groups.size(), false);
    std::vector<uint32_t> queue;
    queue.reserve(model.groups.size());

    auto seed = [&](int gi) {
        if (gi < 0 || gi >= static_cast<int>(model.groups.size())) return;
        if (visited[gi]) return;
        visited[gi] = true;
        queue.push_back(static_cast<uint32_t>(gi));
        outVisibleGroups.insert(static_cast<uint32_t>(gi));
    };
    seed(cameraGroup);
    seed(findContainingGroup(model, viewerLocalPos));

    // Exterior groups were inserted above without being queued; they are portals
    // into the interior too.
    for (uint32_t gi : outVisibleGroups) {
        if (!visited[gi]) {
            visited[gi] = true;
            queue.push_back(gi);
        }
    }

    size_t queueIdx = 0;
    while (queueIdx < queue.size()) {
        uint32_t currentGroup = queue[queueIdx++];

        // Get portal refs for this group
        if (currentGroup >= model.groupPortalRefs.size()) continue;
        auto [portalStart, portalCount] = model.groupPortalRefs[currentGroup];

        for (uint16_t pi = 0; pi < portalCount; pi++) {
            uint16_t refIdx = portalStart + pi;
            if (refIdx >= model.portalRefs.size()) continue;

            const auto& ref = model.portalRefs[refIdx];
            uint32_t targetGroup = ref.groupIndex;

            if (targetGroup >= model.groups.size()) continue;
            if (visited[targetGroup]) continue;

            // Check if portal is visible from camera
            if (isPortalVisible(model, ref.portalIndex, cameraLocalPos, frustum, modelMatrix)) {
                visited[targetGroup] = true;
                outVisibleGroups.insert(targetGroup);
                queue.push_back(targetGroup);
            }
        }
    }
}

void WMORenderer::WMOInstance::updateModelMatrix() {
    // The placement rotation the caller stored as (C, A, B + 180) in radians.
    // Buildings and doodads compose this identically, in placement_transform.hpp,
    // which records how the order was solved against the bounding boxes MODF
    // carries: composing it in both places is how they came to disagree, and
    // how a building on flat ground could look right while a bridge across a
    // ravine did not.
    modelMatrix = pipeline::placementModelMatrix(position, rotation, scale);

    // Cache inverse for collision detection
    invModelMatrix = glm::inverse(modelMatrix);
}

EncodedNormalMap WMORenderer::prepareNormalHeightMap(const uint8_t* pixels, uint32_t width, uint32_t height,
                                                     NormalMapFormat format) {
    // Two, where the character renderer asks for five: stone and wood at the
    // distance a wall is seen from, which a stronger gradient only roughens.
    float variance = 0.0f;
    std::vector<uint8_t> output = rendering::generateNormalHeightMap(
        pixels, width, height, kWmoNormalMapStrength, variance);
    if (output.empty()) return {};
    return encodeNormalMap(std::move(output), width, height, format, variance);
}

NormalMapFormat WMORenderer::normalMapFormat() const {
    return vkCtx_ ? normalMapFormatFor(*vkCtx_) : NormalMapFormat::Rgba8;
}

TextureRef WMORenderer::loadNormalHeightMap(const std::string& resolvedPath,
                                            const pipeline::BLPImage* decoded) {
    if (!normalMappingEnabled_ && !pomEnabled_) return {};
    TextureCache& cache = vkCtx_->textures();
    const TextureKey key{resolvedPath, TextureForm::NormalHeight, kWmoNormalMapStrengthKey};
    if (TextureRef cached = cache.find(key)) return cached;

    // Prefer what the terrain workers made alongside the decoded diffuse: the
    // Sobel/blur pass then never runs on the render thread.
    if (predecodedNormalMapCache_) {
        auto normalIt = predecodedNormalMapCache_->find(resolvedPath);
        if (normalIt != predecodedNormalMapCache_->end()) {
            const EncodedNormalMap prepared = std::move(normalIt->second);
            predecodedNormalMapCache_->erase(normalIt);
            if (prepared.valid()) return cacheNormalMap(*vkCtx_, key, prepared);
        }
    }
    // Streaming defers what the workers did not prepare: the building draws
    // flat until its textures are next loaded, rather than stall the frame.
    if (deferNormalMaps_) return {};

    // Made here, from the file decoded and thrown away rather than kept. The
    // diffuse may have come from the cache - another renderer's load - in
    // which case the file is read again for its pixels.
    pipeline::BLPImage file;
    if (!decoded) {
        file = assetManager->loadTexture(resolvedPath, true);
        if (!file.isValid()) return {};
        decoded = &file;
    }
    const std::vector<uint8_t> pixels =
        decoded->isBlockCompressed() ? pipeline::BLPLoader::decodeBaseLevel(*decoded) : decoded->data;
    if (pixels.empty()) return {};
    const EncodedNormalMap prepared = prepareNormalHeightMap(
        pixels.data(), static_cast<uint32_t>(decoded->width), static_cast<uint32_t>(decoded->height),
        normalMapFormat());
    if (!prepared.valid()) return {};
    return cacheNormalMap(*vkCtx_, key, prepared);
}

WMORenderer::LoadedTexture WMORenderer::loadTexture(const std::string& path) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    if (!assetManager || !vkCtx_) {
        return {whiteTexture_, {}};
    }

    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    std::string key = path;
    // Some assets contain stray bytes after a NUL in path chunks.
    size_t nul = key.find('\0');
    if (nul != std::string::npos) key.resize(nul);
    key = normalizeKey(key);
    if (key.rfind(".\\", 0) == 0) key = key.substr(2);
    while (!key.empty() && key.front() == '\\') key.erase(key.begin());
    if (key.empty()) return {whiteTexture_, {}};

    auto hasKnownExt = [](const std::string& p) {
        if (p.size() < 4) return false;
        std::string ext = p.substr(p.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return (ext == ".blp" || ext == ".tga" || ext == ".dds");
    };
    auto toBlp = [](std::string p) {
        if (p.size() >= 4) {
            std::string ext = p.substr(p.size() - 4);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".tga" || ext == ".dds") {
                p = p.substr(0, p.size() - 4) + ".blp";
            }
        }
        return p;
    };

    std::vector<std::string> candidates;
    auto addCandidate = [&](const std::string& raw) {
        std::string c = normalizeKey(raw);
        if (c.rfind(".\\", 0) == 0) c = c.substr(2);
        while (!c.empty() && c.front() == '\\') c.erase(c.begin());
        if (!c.empty()) candidates.push_back(c);
    };

    addCandidate(toBlp(key));
    if (!hasKnownExt(key)) addCandidate(key + ".blp");

    // Common WMO references omit folder prefix; manifest often stores these under textures\...
    std::string keyWithExt = hasKnownExt(key) ? toBlp(key) : (key + ".blp");
    if (key.find('\\') == std::string::npos) {
        addCandidate(std::string("textures\\") + keyWithExt);
    }
    if (key.rfind("texture\\", 0) == 0) {
        addCandidate(std::string("textures\\") + key.substr(8));
    }

    // De-duplicate while preserving order.
    std::vector<std::string> uniqueCandidates;
    uniqueCandidates.reserve(candidates.size());
    std::unordered_set<std::string> seen;
    for (const auto& c : candidates) {
        if (seen.insert(c).second) uniqueCandidates.push_back(c);
    }

    // Cache lookup across all candidate keys: the shared cache, so a texture
    // a doodad or the ground already loaded is the same image here.
    TextureCache& cache = vkCtx_->textures();
    for (const auto& c : uniqueCandidates) {
        if (TextureRef cached = cache.find({c, TextureForm::Stored, 0})) {
            return {std::move(cached), loadNormalHeightMap(c, nullptr)};
        }
    }

    const uint64_t lookupSerial = ++textureLookupSerial_;
    std::vector<std::string> attemptedCandidates;
    attemptedCandidates.reserve(uniqueCandidates.size());
    for (const auto& c : uniqueCandidates) {
        auto fit = failedTextureRetryAt_.find(c);
        if (fit != failedTextureRetryAt_.end() && lookupSerial < fit->second) {
            continue;
        }
        attemptedCandidates.push_back(c);
    }
    if (attemptedCandidates.empty()) {
        return {whiteTexture_, {}};
    }

    // Try loading all candidates until one succeeds
    // Check pre-decoded BLP cache first (populated by background worker threads)
    pipeline::BLPImage blp;
    std::string resolvedKey;
    if (predecodedBLPCache_) {
        for (const auto& c : uniqueCandidates) {
            auto pit = predecodedBLPCache_->find(c);
            if (pit != predecodedBLPCache_->end()) {
                blp = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
                resolvedKey = c;
                break;
            }
        }
    }
    if (!blp.isValid()) {
        for (const auto& c : attemptedCandidates) {
            // Blocks. The diffuse is only ever sampled; the normal map
            // below needs pixels and decodes them from these when it runs.
            blp = assetManager->loadTexture(c, true);
            if (blp.isValid()) {
                resolvedKey = c;
                break;
            }
        }
    }
    if (!blp.isValid()) {
        for (const auto& c : attemptedCandidates) {
            failedTextureRetryAt_[c] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (loggedTextureLoadFails_.insert(key).second) {
            core::Logger::getInstance().warning("WMO: Failed to load texture: ", path,
                                                " - drawn white until a retry finds it");
        }
        // Do not cache failures as white. MPQ reads can fail transiently
        // during streaming/contention, and caching white here permanently
        // poisons the texture for this session.
        return {whiteTexture_, {}};
    }

    core::Logger::getInstance().debug("WMO texture: ", path, " size=", blp.width, "x", blp.height);

    // Into the shared cache. Refused when everything resident is in use (the
    // cache says so): asked for again after the retry interval, like a failed
    // read, by retryPendingTextures.
    TextureRef diffuse = cacheStoredTexture(*vkCtx_, resolvedKey, blp);
    if (!diffuse) {
        for (const auto& c : attemptedCandidates) {
            failedTextureRetryAt_[c] = lookupSerial + kFailedTextureRetryLookups;
        }
        return {whiteTexture_, {}};
    }
    failedTextureRetryAt_.erase(resolvedKey);
    core::Logger::getInstance().debug("WMO: Loaded texture: ", path, " (", blp.width, "x", blp.height, ")");
    return {std::move(diffuse), loadNormalHeightMap(resolvedKey, &blp)};
}

void WMORenderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layout)
    destroy(device, opaquePipeline_);
    destroy(device, alphaTestPipeline_);
    destroy(device, transparentPipeline_);
    destroy(device, wireframePipeline_);

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/wmo.vert.spv") ||
        !fragShader.loadFromFile(device, "assets/shaders/wmo.frag.spv")) {
        core::Logger::getInstance().error("WMORenderer::recreatePipelines: failed to load shaders");
        return;
    }

    buildMainPassPipelines(device, vertShader, fragShader);

    vertShader.destroy();
    fragShader.destroy();

    core::Logger::getInstance().info("WMORenderer: pipelines recreated");
}

} // namespace rendering
} // namespace wowee

namespace wowee {
namespace rendering {

void WMORenderer::registerRtModel(uint32_t modelId, const ModelData& model, WmoModelPrep& prep) {
    if (!rtScene_ || rtModelMeshes_.count(modelId)) return;
    // Each material's colour from the texture it has now, then each triangle
    // the preparation kept takes its material's.
    const auto surfaceOf = [&](uint32_t mat) {
        const uint32_t blend = mat < model.materialBlendModes.size() ? model.materialBlendModes[mat] : 0;
        glm::vec3 albedo(0.5f);
        float opacity = 1.0f;
        if (mat < model.materialTextureIndices.size()) {
            const uint32_t ti = model.materialTextureIndices[mat];
            if (ti < model.textures.size() && model.textures[ti]) {
                albedo = model.textures[ti]->averageColor();
                if (blend == 1) opacity = model.textures[ti]->alphaCoverage();
            }
        }
        return packRtSurface(albedo, opacity);
    };
    std::vector<float> materialSurface(model.materialBlendModes.size());
    for (uint32_t m = 0; m < materialSurface.size(); ++m) materialSurface[m] = surfaceOf(m);
    const float unknownSurface = surfaceOf(UINT32_MAX);
    RtScene::MeshSource src = std::move(prep.rtMesh);
    src.surfaces.resize(prep.rtTriangleMaterial.size());
    for (size_t t = 0; t < prep.rtTriangleMaterial.size(); ++t) {
        const uint32_t m = prep.rtTriangleMaterial[t];
        src.surfaces[t] = m < materialSurface.size() ? materialSurface[m] : unknownSurface;
    }
    const RtScene::MeshId mesh = rtScene_->addMesh(std::move(src));
    if (mesh != RtScene::kInvalid) {
        rtModelMeshes_[modelId] = mesh;
        scene_->slots.modelChanged(modelId);
    }
}

void WMORenderer::releaseRtModel(uint32_t modelId) {
    if (!rtScene_) return;
    scene_->rtChanges.releaseModel(scene_->slots, modelId, rtScene_);
    auto mesh = rtModelMeshes_.find(modelId);
    if (mesh == rtModelMeshes_.end()) return;
    rtScene_->removeMesh(mesh->second);
    rtModelMeshes_.erase(mesh);
}

void WMORenderer::syncRtScene() {
    if (!rtScene_) return;
    scene_->rtChanges.flush(scene_->slots, *rtScene_, [&](uint32_t id) {
        const auto& inst = scene_->instances[scene_->instanceIndexById.at(id)];
        const auto mesh = rtModelMeshes_.find(inst.modelId);
        return RtInstanceChanges::State{
            !inst.hidden && mesh != rtModelMeshes_.end() ? mesh->second : RtScene::kInvalid,
            inst.modelMatrix};
    }, [](uint32_t id, uint32_t mesh) {
        LOG_WARNING("WMO instance ", id, " could not register RT mesh ", mesh,
                    "; drawn without ray-traced occlusion until its source changes");
    });
}

} // namespace rendering
} // namespace wowee

namespace wowee::rendering {
WMORenderer::InstanceSession::InstanceSession(WMORenderer& renderer)
    : renderer_(renderer) {
    if (renderer_.scene_ != &renderer_.defaultInstances_)
        throw std::logic_error("WMORenderer already has a scene owner");
    state_ = std::make_unique<InstanceState>(std::move(renderer_.defaultInstances_));
    renderer_.defaultInstances_ = InstanceState{};
    renderer_.scene_ = state_.get();
}

WMORenderer::InstanceSession::~InstanceSession() {
    renderer_.clearInstances();
    renderer_.scene_ = &renderer_.defaultInstances_;
}
}
