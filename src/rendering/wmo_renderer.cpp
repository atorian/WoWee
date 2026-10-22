#include "collision/collision_world.hpp"
#include "collision/geometry.hpp"
#include "pipeline/heap_bytes.hpp"
#include "rendering/pom_quality.hpp"
#include "rendering/placement_transform.hpp"
#include "rendering/wmo_vertex.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/wmo_renderer.hpp"
#include <stdexcept>
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/wmo_batch_merge.hpp"
#include "wmo_model_prep.hpp"
#include "rendering/wmo_lighting.hpp"
#include "rendering/wmo_deformation_bounds.hpp"
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
// The water reflection draws through these same pipelines with a mirrored
// view, which reverses every triangle there. That pass only draws the scene
// with multisampling off.
constexpr VkFrontFace kWmoFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

// Where an alpha-keyed WMO material (MOMT blend mode 1) is cut, in 255ths: the
// original client's 224, which wmo.frag uses too. The shadow pass is handed it
// so a cutout casts the same shape it draws.
constexpr int kWmoAlphaKeyRef = 224;
} // namespace

using collision::rayIntersectsAABB;
using collision::transformAABB;

WMORenderer::WMORenderer() {
}

WMORenderer::~WMORenderer() {
    shutdown();
}

/// Builds the three main-pass pipelines from an already-loaded shader pair.
///
/// initialize() and recreatePipelines() both need exactly these three, in
/// exactly these states, and each described all three for itself. They only
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
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)
        .build(device, vkCtx_->getPipelineCache());

    if (!opaquePipeline_) {
        core::Logger::getInstance().error("WMORenderer: failed to create opaque pipeline");
        return false;
    }

    // --- Build transparent pipeline (derivative of opaque) ---
    transparentPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, kWmoFrontFace)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());

    if (!transparentPipeline_) {
        core::Logger::getInstance().warning("WMORenderer: transparent pipeline not available");
    }

    // --- Build wireframe pipeline (derivative of opaque) ---
    wireframePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE, kWmoFrontFace)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
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

    // --- Create material descriptor set layout (set 1) ---
    // binding 0: sampler2D (diffuse texture)
    // binding 1: uniform buffer (WMOMaterial)
    // binding 2: sampler2D (normal+height map)
    std::vector<VkDescriptorSetLayoutBinding> materialBindings(3);
    materialBindings[0] = {};
    materialBindings[0].binding = 0;
    materialBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[0].descriptorCount = 1;
    materialBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[1] = {};
    materialBindings[1].binding = 1;
    materialBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    materialBindings[1].descriptorCount = 1;
    materialBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[2] = {};
    materialBindings[2].binding = 2;
    materialBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[2].descriptorCount = 1;
    materialBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    materialSetLayout_ = createDescriptorSetLayout(device, materialBindings);
    if (!materialSetLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create material set layout");
        return false;
    }

    // --- Create descriptor pool ---
    VkDescriptorPoolSize poolSizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_MATERIAL_SETS * 2 },  // diffuse + normal/height
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_MATERIAL_SETS },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = MAX_MATERIAL_SETS;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &materialDescPool_) != VK_SUCCESS) {
        core::Logger::getInstance().error("WMORenderer: failed to create descriptor pool");
        return false;
    }
    materialSlots_ = UniformSlotPool::forDevice(vkCtx_->getAllocator(), vkCtx_->getPhysicalDevice(),
                                                sizeof(WMOMaterialUBO), 1024, "WMO materials");

    // --- Create pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(WMOPushConstants);

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, materialSetLayout_ };
    // Vertex data occupies the first 96 bytes. Fragment settings come from
    // the extracted frame and never update any material descriptor or buffer.
    static_assert(sizeof(WMOPushConstants) == 96);
    VkPushConstantRange surfaceRange{};
    surfaceRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    surfaceRange.offset = sizeof(WMOPushConstants);
    surfaceRange.size = sizeof(glm::vec4);
    pipelineLayout_ = createPipelineLayout(device, setLayouts, { pushRange, surfaceRange });
    surfaceRange.size = 32;
    surfaceLayout_ = createPipelineLayout(device, setLayouts, { pushRange, surfaceRange });
    if (!pipelineLayout_ || !surfaceLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create pipeline layout");
        return false;
    }

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/wmo.vert.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, (vkCtx_->hardwareRayQueryEnabled() ? "assets/shaders/wmo_hw.frag.spv" : "assets/shaders/wmo.frag.spv"))) {
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
    VmaAllocator allocator = vkCtx_->getAllocator();

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

    // destroyGroupGPU defers its frees, and those lambdas release descriptor
    // sets from the pool destroyed just below. Drain them here, while the pool
    // is still valid and before the pipelines and pools go: no further frames
    // will run to drain the queue, so anything left in it either leaks or is
    // run later against a dead pool.
    vkCtx_->flushDeferredCleanup();
    if (materialSlots_) materialSlots_->release();

    // Destroy pipelines
    surfacePass_=nullptr;
    destroy(device, surfacePipeline_);
    surfacePipelinePass_ = VK_NULL_HANDLE;
    destroy(device, surfaceLayout_);
    destroy(device, opaquePipeline_);
    destroy(device, transparentPipeline_);
    destroy(device, wireframePipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, materialDescPool_);
    destroy(device, materialSetLayout_);

    // Destroy shadow resources
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    shadowTexSetCache_.clear();
    for (auto& pool : shadowTexPool_) destroy(device, pool);
    destroyShadowParamsSet(device, allocator, shadowParams_);

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

        AllocatedBuffer vertBuf = uploadBuffer(*vkCtx_, prepared.vertices.data(),
            prepared.vertices.size() * sizeof(WMOVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        resources.vertexBuffer = vertBuf.buffer;
        resources.vertexAlloc = vertBuf.allocation;
        AllocatedBuffer idxBuf = uploadBuffer(*vkCtx_, prepared.indices.data(),
            prepared.indices.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        resources.indexBuffer = idxBuf.buffer;
        resources.indexAlloc = idxBuf.allocation;
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
            mb.pendingTextureIndex = pendingTexture;
            mb.hasTexture = tex != whiteTexture_.get();
            mb.alphaTest = state.blendMode == 1;
            mb.isTransparent = state.blendMode >= 2;
            mb.sidnColor = state.sidnColor;
            mb.isLava = state.isLava;
            mb.emissiveLevel = state.emissiveLevel;
            mb.clothTop = state.clothTop;
            mb.clothDrop = state.clothDrop;
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
        shape->groups.reserve(modelData.groups.size());
        for (const auto& group : modelData.groups) shape->groups.push_back(group.shape);
        modelData.shape = std::move(shape);
    }
    loadedModels[id] = std::move(modelData);
    scene_->loadingModels_.erase(id);
    // Instances placed while it loaded had no groups to answer from.
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
        destroyGroupGPU(group, /*defer=*/true);
    }
    // Its groups, GPU handles handed to the deferred deletion above, are
    // collision geometry and grids now - the bulk of a city model, let go on
    // the background thread. Its textures go here: the cache they are shared
    // through is the main thread's.
    core::releaseInBackground(
        std::make_unique<std::vector<GroupResources>>(std::move(it->second.groups)));

    loadedModels.erase(it);
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
                                     const glm::vec3& rotation, float scale, uint32_t reservedId) {
    // Check if model is loaded
    if (loadedModels.find(modelId) == loadedModels.end()) {
        core::Logger::getInstance().error("Cannot create instance of unloaded WMO model ", modelId);
        return 0;
    }

    WMOInstance instance;
    instance.surfaceLifetime = nextSurfaceIdentity();
    instance.id = reservedId != 0 ? reservedId : nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;
    instance.updateModelMatrix();
    const ModelData& model = loadedModels[modelId];
    refreshInstanceBounds(instance);

    scene_->instances.push_back(instance);
    size_t idx = scene_->instances.size() - 1;
    scene_->instanceIndexById[instance.id] = idx;
    scene_->rtChanges.add(instance.id, modelId);
    if (collision_) collision_->placeWmo(instance.id, model.shape, instance.modelMatrix);
    markContentChanged();
    core::Logger::getInstance().debug("Created WMO instance ", instance.id, " (model ", modelId, ")");
    return instance.id;
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
        float displacement=0;
        for (const auto& batch : group.mergedBatches)
            displacement=std::max(displacement,wmoClothDisplacementBound(batch.clothDrop));
        transformAABB(inst.modelMatrix, group.boundingBoxMin-glm::vec3(displacement),
                      group.boundingBoxMax+glm::vec3(displacement), gMin, gMax);
        inst.worldBoundsMin=glm::min(inst.worldBoundsMin,gMin);
        inst.worldBoundsMax=glm::max(inst.worldBoundsMax,gMax);
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
    scene_->rtChanges.changed(instanceId);
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
    scene_->rtChanges.changed(instanceId);

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
        scene_->rtChanges.remove(instanceId, rtScene_);
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

    for (uint32_t id : toRemove) scene_->rtChanges.remove(id, rtScene_);
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
        for (auto& group : model.groups) destroyGroupGPU(group, true);
    }
    *scene_ = InstanceState{};
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
                destroyGroupGPU(group, /*defer=*/true);
            }
        }

        // Deferred groups return individual descriptor sets; keep their pool.

    }

    // The models' textures go back to the shared cache with them, to be found
    // again by the next map or evicted when room is wanted.
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

WMORenderer::GroupDraw WMORenderer::extractGroup(const GroupResources& group,
                                               const WMOInstance& instance, uint32_t index) const {
    GroupDraw out;
    if (!group.drawData) {
        auto data = std::make_shared<GroupDrawData>();
        data->vertexBuffer = group.vertexBuffer;
        data->indexBuffer = group.indexBuffer;
        data->boundingBoxMin = group.boundingBoxMin;
        data->boundingBoxMax = group.boundingBoxMax;
        data->groupFlags = group.groupFlags;
        data->isLOD = group.isLOD;
        data->mergedBatches = group.mergedBatches;
        data->surfaceBatchBase = nextSurfaceIdentity(group.mergedBatches.size());
        group.drawData = std::move(data);
    }
    out.gpu = group.drawData;
    if (index < instance.worldGroupBounds.size()) {
        out.worldMin = instance.worldGroupBounds[index].first;
        out.worldMax = instance.worldGroupBounds[index].second;
    } else {
        const glm::vec3 centre = glm::vec3(instance.modelMatrix * glm::vec4(
            (group.boundingBoxMin + group.boundingBoxMax) * 0.5f, 1.0f));
        const float radius = 0.5f * glm::length(group.boundingBoxMax - group.boundingBoxMin) * instance.scale;
        out.worldMin = centre - glm::vec3(radius);
        out.worldMax = centre + glm::vec3(radius);
    }
    return out;
}

void WMORenderer::Frame::clear() noexcept {
    groups.clear();
    for (auto* list : {&view, &reflection, &depth}) {
        // Keep each entry's group capacity, but no resource from the completed
        // frame. Entries past `used` can belong to an earlier, larger scene.
        for (auto& entry : list->entries) entry.groups.clear();
        list->portalGroups.clear();
        list->clear();
    }
}

std::shared_ptr<const WMORenderer::Frame> WMORenderer::extract(const Camera& camera,
        const glm::vec3& viewer, const std::vector<LocalLight>& lights, const SceneView* reflection) {
    auto frame = framePool_.acquire();
    const SceneView primary = SceneView::fromCamera(camera);
    Frustum primaryFrustum, reflectionFrustum;
    primaryFrustum.extractFromMatrix(primary.viewProjection());
    frame->view.cameraPosition = primary.position;
    if (reflection) {
        reflectionFrustum.extractFromMatrix(reflection->viewProjection());
        frame->reflection.cameraPosition = reflection->position;
    }
    // Visit each placement/group once. Pass lists retain indices into one
    // immutable evaluated record; neither reflection nor shadows rebuild it.
    for (const auto& instance : scene_->instances) {
        if (instance.hidden) continue;
        const auto found = loadedModels.find(instance.modelId);
        if (found == loadedModels.end()) continue;
        const auto& model = found->second;
        const bool primaryPortals = cullPortals(model, instance, primary, viewer,
                                                primaryFrustum, frame->view);
        const bool reflectionPortals = reflection && cullPortals(model, instance, *reflection,
            reflection->position, reflectionFrustum, frame->reflection);
        auto& viewEntry = frame->view.add();
        auto& depthEntry = frame->depth.add();
        DrawEntry* reflectionEntry = reflection ? &frame->reflection.add() : nullptr;
        for (auto* entry : {&viewEntry, &depthEntry, reflectionEntry}) if (entry) {
            entry->surfaceLifetime = instance.surfaceLifetime;
            entry->inverseModelMatrix = instance.invModelMatrix;
            entry->modelMatrix = instance.modelMatrix;
            entry->scale = instance.scale;
        }
        for (uint32_t gi = 0; gi < model.groups.size(); ++gi) {
            const auto& group = model.groups[gi];
            if (!group.vertexBuffer || !group.indexBuffer || (group.groupFlags & 0x4000000)) continue;
            auto kept = extractGroup(group, instance, gi);
            const bool inView = keepGroup(kept, instance, gi, primaryPortals, primaryFrustum, frame->view);
            const bool inReflection = reflection && keepGroup(kept, instance, gi, reflectionPortals,
                                                               reflectionFrustum, frame->reflection);
            const bool inDepth = !group.isLOD;
            if (!inView && !inReflection && !inDepth) continue;
            // Shadow-only records need geometry and bounds, never light selection.
            if (inView || inReflection)
                kept.lights = pickLocalLights(lights, (kept.worldMin + kept.worldMax) * 0.5f,
                                               glm::length(kept.worldMax - kept.worldMin) * 0.5f);
            const uint32_t index = static_cast<uint32_t>(frame->groups.size());
            frame->groups.push_back(std::move(kept));
            if (inView) {
                viewEntry.groups.push_back(index);
                frame->view.surfaceMaterialCapacity+=group.mergedBatches.size();
            }
            if (inReflection) reflectionEntry->groups.push_back(index);
            if (inDepth) depthEntry.groups.push_back(index);
        }
        if (viewEntry.groups.empty()) frame->view.dropLast();
        if (depthEntry.groups.empty()) frame->depth.dropLast();
        if (reflectionEntry && reflectionEntry->groups.empty()) frame->reflection.dropLast();
    }
    frame->wireframe = wireframeMode;
    frame->normalMap = normalMappingEnabled_ ? 1 : 0;
    frame->pom = pomEnabled_ ? 1 : 0;
    frame->samples = pomSamplesFor(pomQuality_);
    frame->normalStrength = normalMapStrength_;
    return frame;
}

void WMORenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera&,
                         const glm::vec3*, Pass pass) {
    if (!opaquePipeline_) return;
    const auto& list = pass == Pass::Reflection ? recording_->reflection : recording_->view;
    const uint32_t draws = recordView(cmd, perFrameSet, list.cameraPosition, list);
    if (pass == Pass::View) viewDrawCallCount_ = draws;
}

void WMORenderer::prepareSurface(SurfacePass& pass) {
    if (recording_->wireframe) {
        LOG_ERROR("WMO evaluated-surface wireframe is not implemented - refusing mismatched rendering");
        throw std::runtime_error("WMO surface wireframe unsupported");
    }
    if (recording_->view.surfaceMaterialCapacity == 0) return;
    surfacePass_=&pass;
    if (surfacePipelinePass_!=surfacePass_->geometryPass()) {
        DeferredResources retire(*vkCtx_);retire.destroy(surfacePipeline_);
        VkShaderModule vert,frag;
        if (!vert.loadFromFile(vkCtx_->getDevice(),"assets/shaders/wmo.vert.spv") ||
            !frag.loadFromFile(vkCtx_->getDevice(),"assets/shaders/wmo_surface.frag.spv")) {
            LOG_ERROR("WMO surface shaders unavailable - frame cannot be rendered");
            throw std::runtime_error("WMO surface shaders unavailable");
        }
        surfacePipeline_=PipelineBuilder().setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({perVertexBinding(sizeof(WMOVertex))},toVkAttributes(kWmoVertexAttributes))
            .setRasterization(VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE,kWmoFrontFace)
            .setDepthTest(true,true,VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachments(std::vector<VkPipelineColorBlendAttachmentState>(3,PipelineBuilder::blendDisabled()))
            .setMultisample(vkCtx_->getMsaaSamples()).setLayout(surfaceLayout_)
            .setRenderPass(surfacePass_->geometryPass()).setDynamicStates(viewportAndScissorDynamic())
            .build(vkCtx_->getDevice(),vkCtx_->getPipelineCache());
        vert.destroy();frag.destroy();
        if (!surfacePipeline_) {
            LOG_ERROR("WMO surface pipeline creation failed - frame cannot be rendered");
            throw std::runtime_error("WMO surface pipeline creation failed");
        }
        surfacePipelinePass_=surfacePass_->geometryPass();
    }
}

void WMORenderer::recordSurface(VkDescriptorSet perFrameSet, const Camera& camera) {
    viewDrawCallCount_ = 0;
    if (recording_->view.surfaceMaterialCapacity == 0) return;
    const glm::vec4 options(float(recording_->normalMap),float(recording_->pom),float(recording_->samples),recording_->normalStrength);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ::VkBuffer boundVertex = VK_NULL_HANDLE, boundIndex = VK_NULL_HANDLE;
    VkDescriptorSet boundMaterial = VK_NULL_HANDLE;
    bool geometry = false;
    uint32_t draws = 0;
    previousSurfaceTransforms_.swap(currentSurfaceTransforms_);
    currentSurfaceTransforms_.clear();
    const bool previousValid=surfacePass_->rtHistoryValid() && surfaceRecordedFrame_+1==vkCtx_->frameNumber();
    for (const auto& entry : recording_->view.drawn()) {
        const auto previous=previousSurfaceTransforms_.find(entry.surfaceLifetime);
        const bool motionValid=previousValid && previous!=previousSurfaceTransforms_.end();
        glm::mat4 previousFromCurrent(1);
        if(motionValid) {
            auto currentToObject=entry.inverseModelMatrix;
            currentToObject[3]=entry.inverseModelMatrix*glm::vec4(camera.getPosition(),1);
            auto previousRelative=previous->second;
            previousRelative[3]-=glm::vec4(surfacePass_->previousRtCamera(),0);
            previousFromCurrent=previousRelative*currentToObject;
        }
        currentSurfaceTransforms_.insert_or_assign(entry.surfaceLifetime,entry.modelMatrix);
        for (const uint32_t index : entry.groups) {
            const auto& kept=recording_->groups[index];
            const auto& group=*kept.gpu;
            if (group.groupFlags & 0x4000000) continue;
            if (group.isLOD) {
                const glm::vec3 center=entry.modelMatrix*glm::vec4((group.boundingBoxMin+group.boundingBoxMax)*0.5f,1);
                if (glm::dot(center-camera.getPosition(),center-camera.getPosition())<196.f*196.f) continue;
            }
            if (!group.vertexBuffer || !group.indexBuffer) continue;
            uint64_t batchIdentity=group.surfaceBatchBase;
            for (const auto& batch:group.mergedBatches) {
                const uint64_t identity=batchIdentity++;
                if (!batch.materialSet) continue;
                const bool opaque=!batch.isTransparent;
                if (!opaque && !transparentPipeline_) {
                    LOG_ERROR("WMO blended pipeline unavailable - surface frame cannot be rendered");
                    throw std::runtime_error("WMO blended pipeline unavailable");
                }
                const auto layout=opaque?surfaceLayout_:pipelineLayout_;
                if (!cmd || opaque != geometry) {
                    if (cmd) surfacePass_->endRun(SurfacePass::Family::Wmo);
                    cmd = surfacePass_->beginRun(SurfacePass::Family::Wmo,opaque);
                    geometry = opaque;
                    boundVertex = boundIndex = VK_NULL_HANDLE;
                    boundMaterial = VK_NULL_HANDLE;
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        opaque ? surfacePipeline_ : transparentPipeline_);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        layout, 0, 1, &perFrameSet, 0, nullptr);
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        96, sizeof(options), &options);
                }
                if (boundVertex != group.vertexBuffer) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &group.vertexBuffer, &offset);
                    boundVertex = group.vertexBuffer;
                }
                if (boundIndex != group.indexBuffer) {
                    vkCmdBindIndexBuffer(cmd, group.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
                    boundIndex = group.indexBuffer;
                }
                if (boundMaterial != batch.materialSet) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        layout, 1, 1, &batch.materialSet, 0, nullptr);
                    boundMaterial = batch.materialSet;
                }
                WMOPushConstants push{};push.model=entry.modelMatrix;push.localLights=kept.lights;
                push.cloth={batch.clothTop,batch.clothDrop,batch.clothCentreX,batch.clothCentreY};
                vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
                if (opaque) {
                    SurfacePass::Material material{};
                    static_assert(sizeof(batch.material)==sizeof(material.words));
                    std::memcpy(material.words.data(),&batch.material,sizeof(batch.material));
                    material.localLights=kept.lights;
                    material.previousFromCurrent=previousFromCurrent;
                    material.identity={uint32_t(entry.surfaceLifetime),uint32_t(entry.surfaceLifetime>>32),uint32_t(identity),uint32_t(identity>>32)};
                    material.history.x=motionValid && batch.clothDrop==0.0f ? 1u : 0u;
                    const glm::uvec4 key(surfacePass_->appendMaterial(material),0,0,0);
                    vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,112,sizeof(key),&key);
                }
                for (const auto& range:batch.draws) if (range.indexCount) {
                    vkCmdDrawIndexed(cmd,range.indexCount,1,range.firstIndex,0,0);++draws;
                }
            }
        }
    }
    if (cmd) surfacePass_->endRun(SurfacePass::Family::Wmo);
    viewDrawCallCount_=draws;
    surfaceRecordedFrame_=vkCtx_->frameNumber();
}


bool WMORenderer::cullPortals(const ModelData& model, const WMOInstance& instance,
        const SceneView& camera, const glm::vec3& portalViewerPos,
        const Frustum& frustum, DrawList& list) const {
    const glm::vec3 camPos = camera.position;
    const bool doPortalCull = portalCulling && cullingEnabled_;
    // Portal-based visibility, into the pass's own scratch set.
    bool usePortalCulling = doPortalCull && !model.portals.empty() && !model.portalRefs.empty();
    const glm::vec3 localRealCam =
        glm::vec3(instance.invModelMatrix * glm::vec4(camPos, 1.0f));
    if (usePortalCulling) {
        // If the camera is outside all groups, skip portal culling entirely.
        // This is what makes it safe for the traversal below to start from
        // the camera: an orbiting third-person camera that has left the
        // building never reaches the walk at all.
        int camGroup = findContainingGroup(model, localRealCam);
        if (camGroup < 0) {
            usePortalCulling = false;
        } else {
            // Entranceways and awnings: the best-fit AABB often claims an
            // interior group while the camera is visually outside (interior
            // boxes spill past the doorway). Only trust portal traversal
            // when the camera group is interior-only - the same rule
            // getVisibleGroupsViaPortals applies to the viewer position.
            constexpr uint32_t WMO_GROUP_FLAG_OUTDOOR = 0x8;
            constexpr uint32_t WMO_GROUP_FLAG_INDOOR = 0x2000;
            const uint32_t gFlags = model.groups[camGroup].groupFlags;
            const bool isIndoor = (gFlags & WMO_GROUP_FLAG_INDOOR) != 0;
            const bool isOutdoor = (gFlags & WMO_GROUP_FLAG_OUTDOOR) != 0;
            if (!isIndoor || isOutdoor) {
                usePortalCulling = false;
            } else {
                // Doorway thresholds sit inside both the interior box and
                // an outdoor street group's box - treat those as outdoors
                // too (second half of the viewer-side rule).
                for (size_t gi = 0; gi < model.groups.size(); ++gi) {
                    if (static_cast<int>(gi) == camGroup) continue;
                    const auto& g = model.groups[gi];
                    if (!(g.groupFlags & WMO_GROUP_FLAG_OUTDOOR)) continue;
                    if (localRealCam.x >= g.boundingBoxMin.x && localRealCam.x <= g.boundingBoxMax.x &&
                        localRealCam.y >= g.boundingBoxMin.y && localRealCam.y <= g.boundingBoxMax.y &&
                        localRealCam.z >= g.boundingBoxMin.z && localRealCam.z <= g.boundingBoxMax.z) {
                        usePortalCulling = false;
                        break;
                    }
                }
            }
        }
    }
    if (usePortalCulling) {
        list.portalGroups.clear();
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
        const glm::vec3 localViewer =
            glm::vec3(instance.invModelMatrix * glm::vec4(portalViewerPos, 1.0f));
        getVisibleGroupsViaPortals(model, localRealCam, localViewer, frustum,
                                   instance.modelMatrix, list.portalGroups);
        // Use the unordered_set directly - was copying into portalVisibleGroups_,
        // sorting it, and binary-searching per group. The set lookup is O(1)
        // per group and skips the per-instance copy + sort.
    }

    return usePortalCulling;
}

bool WMORenderer::keepGroup(const GroupDraw& group, const WMOInstance& instance,
        uint32_t index, bool usePortals, const Frustum& frustum, DrawList& list) const {
    if (usePortals && !list.portalGroups.contains(index)) {
        ++list.portalCulled;
        return false;
    }
    const auto camPos = list.cameraPosition;
    if (index < instance.worldGroupBounds.size()) {
        const auto& [gMin, gMax] = instance.worldGroupBounds[index];
        const glm::vec2 nearest = glm::clamp(glm::vec2(camPos), glm::vec2(gMin), glm::vec2(gMax));
        const glm::vec2 delta = nearest - glm::vec2(camPos);
        if (glm::dot(delta, delta) > viewDistance_ * viewDistance_) {
            ++list.distanceCulled;
            return false;
        }
        if (cullingEnabled_) {
            const glm::vec3 delta3 = glm::clamp(camPos, gMin, gMax) - camPos;
            const float reach = distanceCulling ? std::min(viewDistance_, maxGroupDistance) : viewDistance_;
            if (glm::dot(delta3, delta3) > reach * reach) {
                ++list.distanceCulled;
                return false;
            }
        }
    }
    if (frustumCulling && !frustum.intersectsAABB(group.worldMin, group.worldMax)) {
        ++list.frustumCulled;
        return false;
    }
    return true;
}

uint32_t WMORenderer::recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const glm::vec3& camPos,
                             const DrawList& list) const {
    if (list.used == 0) return 0;
    uint32_t drawCalls = 0;
    const glm::vec4 surfaceSettings(float(recording_->normalMap), float(recording_->pom),
                                    float(recording_->samples), recording_->normalStrength);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       sizeof(WMOPushConstants), sizeof(surfaceSettings), &surfaceSettings);

    // Select pipeline based on wireframe mode
    VkPipeline activePipeline = (recording_->wireframe && wireframePipeline_) ? wireframePipeline_ : opaquePipeline_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

    // Bind per-frame descriptor set (set 0)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                             0, 1, &perFrameSet, 0, nullptr);

    // Track which pipeline is currently bound: 0=opaque, 1=transparent, 2=glass
    int currentPipelineKind = 0;

    for (const DrawEntry& entry : list.drawn()) {

        // Push model matrix. The cloth half is written per batch below; this
        // sets the instance's matrix and clears the cloth for everything that
        // is not one.
        WMOPushConstants push{};
        push.model = entry.modelMatrix;
        push.cloth = glm::vec4(0.0f);
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                            0, sizeof(WMOPushConstants), &push);
        glm::vec4 pushedCloth(0.0f);

        // LOD shell groups render only beyond this distance squared (190 units)
        static constexpr float LOD_SHELL_DIST_SQ = 196.0f * 196.0f;

        // Render visible groups
        for (const uint32_t groupIndex : entry.groups) {
            const auto& kept = recording_->groups[groupIndex];
            const auto& group = *kept.gpu;

            // Only skip antiportal geometry
            if (group.groupFlags & 0x4000000) continue;

            // Skip distance-only LOD shell groups when camera is close to the group
            if (group.isLOD) {
                glm::vec3 groupCenter = entry.modelMatrix * glm::vec4(
                    (group.boundingBoxMin + group.boundingBoxMax) * 0.5f, 1.0f);
                float groupDistSq = glm::dot(camPos - groupCenter, camPos - groupCenter);
                if (groupDistSq < LOD_SHELL_DIST_SQ) continue;
            }

            // Skip groups with invalid GPU resources
            if (group.vertexBuffer == VK_NULL_HANDLE || group.indexBuffer == VK_NULL_HANDLE) continue;

            // Bind vertex + index buffers
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &group.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, group.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                               offsetof(WMOPushConstants, localLights), sizeof(LocalLightSet), &kept.lights);

            // Render each merged batch
            for (const auto& mb : group.mergedBatches) {
                if (!mb.materialSet) continue;

                // Determine which pipeline this batch needs
                int neededPipeline = 0; // opaque
                if (mb.isTransparent && transparentPipeline_) {
                    neededPipeline = 1; // transparent (alpha blend, no depth write)
                }

                // Switch pipeline if needed (descriptor sets and push constants
                // are preserved across compatible pipeline layout switches)
                if (neededPipeline != currentPipelineKind) {
                    VkPipeline targetPipeline = activePipeline;
                    if (neededPipeline == 1) targetPipeline = transparentPipeline_;
                    if (targetPipeline == VK_NULL_HANDLE) continue;

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, targetPipeline);
                    currentPipelineKind = neededPipeline;
                }

                // Bind material descriptor set (set 1)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                         1, 1, &mb.materialSet, 0, nullptr);

                // Where this batch's cloth hangs, when it is cloth. Written
                // only when it changes: most batches are walls and share the
                // zero that says "not cloth".
                const glm::vec4 wantCloth(mb.clothTop, mb.clothDrop,
                                          mb.clothCentreX, mb.clothCentreY);
                if (wantCloth != pushedCloth) {
                    pushedCloth = wantCloth;
                    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                                       offsetof(WMOPushConstants, cloth),
                                       sizeof(glm::vec4), &pushedCloth);
                }

                // Issue draw calls for each range in this merged batch
                for (const auto& dr : mb.draws) {
                    if (dr.indexCount == 0) continue;
                    vkCmdDrawIndexed(cmd, dr.indexCount, 1, dr.firstIndex, 0, 0);
                    ++drawCalls;
                }
            }
        }
    }
    return drawCalls;
}

bool WMORenderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // The set the shadow pass binds, built the same way for all four renderers.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowParamsUBO),
                               whiteTexture_->getImageView(),
                              whiteTexture_->getSampler(), "WMORenderer", shadowParams_)) {
        return false;
    }

    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("WMORenderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    // Create shadow pipeline layout: set 1 = shadowParams_.layout, push constants = 128 bytes
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix and the flags
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParams_.layout}, {pc});
    if (!shadowPipelineLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/shadow.vert.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
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

void WMORenderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                               const glm::vec3& shadowCenter, float shadowRadius, bool resetDescriptors) {
    if (!shadowPipeline_ || !shadowParams_.set) return;

    // Once a frame, by the frame's first depth-only pass: the sets a later
    // one reuses are still in this frame's command buffer.
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if (resetDescriptors) {
        shadowDrawCalls_ = 0;
        if (frameIndex < kShadowTexPoolFrames && shadowTexPool_[frameIndex]) {
            vkCtx_->resetDescriptorPool(shadowTexPool_[frameIndex]);
        }
        shadowTexSetCache_.clear();
    }

    // WMO shadow cull uses the ortho half-extent (shadow map coverage) rather than
    // the proximity radius so that distant buildings whose shadows reach the player
    // are still rendered into the shadow map.
    const auto& shadowList_ = recording_->depth;
    if (shadowList_.used == 0) return;
    const float radius = std::max(shadowRadius, 180.0f);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
        0, 1, &shadowParams_.set, 0, nullptr);

    for (const DrawEntry& entry : shadowList_.drawn()) {

        // A building does not sway, so the sway slot stays zero.
        ShadowPush push{.lightSpaceModel = lightSpaceMatrix * entry.modelMatrix};
        vkCmdPushConstants(cmd, shadowPipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ShadowPush), &push);
        bool cutoutBound = false;

        for (const uint32_t groupIndex : entry.groups) {
            const auto& kept = recording_->groups[groupIndex];
            const glm::vec3 nearest = glm::clamp(shadowCenter, kept.worldMin, kept.worldMax);
            if (glm::dot(nearest - shadowCenter, nearest - shadowCenter) > radius * radius) continue;
            const auto& group = *kept.gpu;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &group.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, group.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            for (const auto& mb : group.mergedBatches) {
                // An alpha-keyed material casts what it draws: Dalaran's
                // lattice windows, grates, fences. Until its texture is in, it
                // casts nothing rather than the solid quad.
                if (mb.alphaTest) {
                    if (!mb.hasTexture) continue;
                    const VkDescriptorSet texSet = shadowTexSet(mb.texture, frameIndex);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowPipelineLayout_, 0, 1, &texSet, 0, nullptr);
                }
                if (mb.alphaTest != cutoutBound) {
                    cutoutBound = mb.alphaTest;
                    if (!cutoutBound) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            shadowPipelineLayout_, 0, 1, &shadowParams_.set, 0, nullptr);
                    }
                    push.flags = cutoutBound ? glm::ivec4(1, 1, 0, kWmoAlphaKeyRef) : glm::ivec4(0);
                    vkCmdPushConstants(cmd, shadowPipelineLayout_,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(ShadowPush), &push);
                }
                for (const auto& dr : mb.draws) {
                    vkCmdDrawIndexed(cmd, dr.indexCount, 1, dr.firstIndex, 0, 0);
                    ++shadowDrawCalls_;
                }
            }
        }
        if (cutoutBound) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                shadowPipelineLayout_, 0, 1, &shadowParams_.set, 0, nullptr);
        }
    }
}

VkDescriptorSet WMORenderer::shadowTexSet(VkTexture* tex, uint32_t frameIndex) {
    // A texture whose upload failed has a null view; binding it to a sampled
    // descriptor is undefined, so it casts as a solid pane instead.
    if (!tex || !tex->isValid()) return shadowParams_.set;
    if (frameIndex >= kShadowTexPoolFrames || !shadowTexPool_[frameIndex]) return shadowParams_.set;

    const VkImageView view = tex->getImageView();
    if (auto it = shadowTexSetCache_.find(view); it != shadowTexSetCache_.end()) return it->second;

    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = shadowTexPool_[frameIndex];
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &shadowParams_.layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkCtx_->allocateDescriptorSets(ai, &set) != VK_SUCCESS) {
        return shadowParams_.set;
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = view;
    imgInfo.sampler = tex->getSampler();
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = shadowParams_.ubo;
    bufInfo.offset = 0;
    bufInfo.range = sizeof(ShadowParamsUBO);
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imgInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);

    shadowTexSetCache_[view] = set;
    return set;
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

void WMORenderer::destroyGroupGPU(GroupResources& group, bool defer) {
    if (!vkCtx_) return;
    VmaAllocator allocator = vkCtx_->getAllocator();

    if (!defer) {
        // Immediate destruction (safe after vkDeviceWaitIdle)
        destroy(allocator, group.vertexBuffer, group.vertexAlloc);
        destroy(allocator, group.indexBuffer, group.indexAlloc);
        for (auto& mb : group.mergedBatches) {
            if (mb.materialSet) {
                vkCtx_->freeDescriptorSets(materialDescPool_, 1, &mb.materialSet);
                mb.materialSet = VK_NULL_HANDLE;
            }
            if (materialSlots_) materialSlots_->free(mb.materialSlot);
            mb.materialSlot = {};
        }
    } else {
        // Deferred destruction - previous frame's command buffer may still
        // reference these buffers and descriptor sets.
        ::VkBuffer vb = group.vertexBuffer;
        VmaAllocation vbAlloc = group.vertexAlloc;
        ::VkBuffer ib = group.indexBuffer;
        VmaAllocation ibAlloc = group.indexAlloc;
        group.vertexBuffer = VK_NULL_HANDLE;
        group.indexBuffer = VK_NULL_HANDLE;

        // Snapshot material handles (::VkBuffer = raw Vulkan handle, not RAII wrapper)
        struct MatSnapshot { VkDescriptorSet set; UniformSlotPool::Slot slot; };
        std::vector<MatSnapshot> mats;
        mats.reserve(group.mergedBatches.size());
        for (auto& mb : group.mergedBatches) {
            mats.push_back({.set = mb.materialSet, .slot = mb.materialSlot});
            mb.materialSet = VK_NULL_HANDLE;
            mb.materialSlot = {};
        }

        VkDescriptorPool pool = materialDescPool_;
        vkCtx_->deferAfterAllFrameFences([ctx = vkCtx_, allocator, pool, vb, vbAlloc, ib, ibAlloc,
                                      slots = materialSlots_, mats = std::move(mats)]() {
            if (vb) gpuDestroyBuffer(allocator, vb, vbAlloc);
            if (ib) gpuDestroyBuffer(allocator, ib, ibAlloc);
            for (auto& m : mats) {
                if (m.set) {
                    VkDescriptorSet s = m.set;
                    ctx->freeDescriptorSets(pool, 1, &s);
                }
                if (slots) slots->free(m.slot);
            }
        });
    }
}

void WMORenderer::attachNormalHeightMap(const ModelData& model, uint32_t texIndex,
                                        GroupResources::MergedBatch& mb) const {
    if (!mb.texture || mb.texture == whiteTexture_.get()) return;
    if (texIndex >= model.normalMaps.size() || !model.normalMaps[texIndex]) return;
    const TextureRef& normals = model.normalMaps[texIndex];
    mb.normalHeightMap = normals.get();
    mb.heightMapVariance = normals.info().heightVariance;
}

void WMORenderer::createMergedBatchMaterial(const ModelData& model, const GroupResources& group,
                                            GroupResources::MergedBatch& mb) {
    group.drawData.reset();
    // The baseline probe reproduces the lighting from before a portal-interior
    // group could be sunlit at all: every batch of one baked, and unshadowed.
    const bool portalInterior = (group.groupFlags & 0x2000) != 0;
    static const bool baselineProbe = std::getenv("WOWEE_ORGRIMMAR_PROBE") &&
                                      std::getenv("WOWEE_ORGRIMMAR_SHADOW_BASELINE");
    const WmoBatchLighting lighting = baselineProbe && portalInterior
        ? WmoBatchLighting{ .light = WmoLight::VertexColour }
        : mb.lighting;

    // Its parameters, in a slot rather than a buffer of its own.
    mb.materialSlot = materialSlots_ ? materialSlots_->allocate() : UniformSlotPool::Slot{};
    if (!mb.materialSlot) {
        // The pool has said why; a batch with no parameters is not drawn.
        mb.materialSet = VK_NULL_HANDLE;
        return;
    }

    // Write material params
    WMOMaterialUBO matData{};
    matData.hasTexture = mb.hasTexture ? 1 : 0;
    matData.alphaTest = mb.alphaTest ? 1 : 0;
    matData.light = static_cast<int32_t>(lighting.light);
    matData.transitionLight = lighting.transition ? static_cast<int32_t>(lighting.under) : -1;
    matData.receivesExteriorShadows = !baselineProbe &&
        wmoReceivesExteriorShadows(group.groupFlags) ? 1 : 0;
    matData.hasVertexColors = (group.groupFlags & pipeline::kWMOGroupHasVertexColors) != 0 ? 1 : 0;
    matData.specularIntensity = 0.5f;
    // MOMT colours are BGRA bytes.
    matData.sidnR = static_cast<float>((mb.sidnColor >> 16) & 0xffu) / 255.0f;
    matData.sidnG = static_cast<float>((mb.sidnColor >> 8) & 0xffu) / 255.0f;
    matData.sidnB = static_cast<float>(mb.sidnColor & 0xffu) / 255.0f;
    matData.heightMapVariance = mb.heightMapVariance;
    matData.isLava = mb.isLava ? 1 : 0;
    matData.wmoAmbientR = model.wmoAmbientColor.r;
    matData.wmoAmbientG = model.wmoAmbientColor.g;
    matData.wmoAmbientB = model.wmoAmbientColor.b;
    matData.emissive = static_cast<int32_t>(mb.emissiveLevel);
    mb.material = matData;
    memcpy(mb.materialSlot.mapped, &matData, sizeof(matData));

    // Allocate and write descriptor set
    mb.materialSet = allocateMaterialSet();
    // Valid, not merely non-null. descriptorInfo() returns the
    // texture's handles as they are, so one whose upload or view
    // creation failed writes VK_NULL_HANDLE into a live descriptor and
    // declares SHADER_READ_ONLY_OPTIMAL over it - undefined behaviour
    // that reaches an NVIDIA driver as a lost device. See #123.
    //
    // The set is dropped rather than written when even the fallback is
    // unsampleable, and the render pass already skips a batch with no
    // set: an unlit wall costs a wall, a null view costs the device.
    const auto pick = [](VkTexture* wanted, VkTexture* fallback) -> VkTexture* {
        if (wanted && wanted->isValid()) return wanted;
        return (fallback && fallback->isValid()) ? fallback : nullptr;
    };
    VkTexture* texToUse = pick(mb.texture, whiteTexture_.get());
    VkTexture* nhMap = pick(mb.normalHeightMap, flatNormalTexture_.get());
    if ((!texToUse || !nhMap) && mb.materialSet) {
        vkCtx_->freeDescriptorSets(materialDescPool_, 1, &mb.materialSet);
        mb.materialSet = VK_NULL_HANDLE;
    }
    if (!mb.materialSet) return;

    VkDescriptorImageInfo imgInfo = texToUse->descriptorInfo();

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = mb.materialSlot.buffer;
    bufInfo.offset = mb.materialSlot.offset;
    bufInfo.range = sizeof(WMOMaterialUBO);

    VkDescriptorImageInfo nhImgInfo = nhMap->descriptorInfo();

    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = mb.materialSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imgInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = mb.materialSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &bufInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = mb.materialSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &nhImgInfo;

    vkUpdateDescriptorSets(vkCtx_->getDevice(), 3, writes, 0, nullptr);
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

            // The old set and buffer may still be read by a frame in flight, so
            // the batch gets new ones and the old go once every slot has fenced.
            const VkDescriptorSet oldSet = mb.materialSet;
            const UniformSlotPool::Slot oldSlot = mb.materialSlot;
            mb.texture = loaded.diffuse.get();
            model.textures[texIndex] = std::move(loaded.diffuse);
            if (texIndex < model.normalMaps.size()) model.normalMaps[texIndex] = std::move(loaded.normalHeight);
            mb.hasTexture = true;
            mb.pendingTextureIndex = UINT32_MAX;
            attachNormalHeightMap(model, texIndex, mb);
            createMergedBatchMaterial(model, group, mb);
            group.allUntextured = false;
            VkDescriptorPool pool = materialDescPool_;
            vkCtx_->deferAfterAllFrameFences([ctx = vkCtx_, pool, oldSet, oldSlot, slots = materialSlots_]() {
                if (oldSet) {
                    VkDescriptorSet set = oldSet;
                    ctx->freeDescriptorSets(pool, 1, &set);
                }
                if (slots) slots->free(oldSlot);
            });
            core::Logger::getInstance().info("WMO ", model.id, " got texture '",
                                             model.textureNames[texIndex], "' on a retry");
            pending[p] = pending.back();
            pending.pop_back();
        }
        if (pending.empty()) {
            modelsWithPendingTextures_[m] = modelsWithPendingTextures_.back();
            modelsWithPendingTextures_.pop_back();
        } else {
            ++m;
        }
    }
}

VkDescriptorSet WMORenderer::allocateMaterialSet() {
    if (!materialDescPool_ || !materialSetLayout_) return VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialDescPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout_;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkCtx_->allocateDescriptorSets(allocInfo, &set) != VK_SUCCESS) {
        core::Logger::getInstance().warning("WMORenderer: failed to allocate material descriptor set");
        return VK_NULL_HANDLE;
    }
    return set;
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
    modelMatrix = placementModelMatrix(position, rotation, scale);

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
    if (preparedNormalMapCache_) {
        auto normalIt = preparedNormalMapCache_->find(resolvedPath);
        if (normalIt != preparedNormalMapCache_->end()) {
            const auto prepared = std::move(normalIt->second);
            preparedNormalMapCache_->erase(normalIt);
            if (prepared && prepared->valid()) return cacheNormalMap(*vkCtx_, key, *prepared);
        }
    }
    if (predecodedNormalMapCache_) {
        auto ready = predecodedNormalMapCache_->find(resolvedPath);
        if (ready != predecodedNormalMapCache_->end()) {
            auto prepared = std::move(ready->second);
            predecodedNormalMapCache_->erase(ready);
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
    pipeline::PreparedTextureRef prepared;
    std::string resolvedKey;
    if (preparedTextureCache_) {
        for (const auto& c : uniqueCandidates) {
            auto pit = preparedTextureCache_->find(c);
            if (pit != preparedTextureCache_->end()) {
                prepared = std::move(pit->second);
                preparedTextureCache_->erase(pit);
                resolvedKey = c;
                break;
            }
        }
    }
    if (!prepared && predecodedBLPCache_) {
        for (const auto& c : uniqueCandidates) {
            auto ready = predecodedBLPCache_->find(c);
            if (ready != predecodedBLPCache_->end()) {
                prepared = std::make_shared<const pipeline::PreparedTexture>(
                    pipeline::PreparedTexture{std::move(ready->second), 0});
                predecodedBLPCache_->erase(ready);
                resolvedKey = c;
                break;
            }
        }
    }
    if (!prepared || !prepared->image.isValid()) {
        for (const auto& c : attemptedCandidates) {
            // Blocks. The diffuse is only ever sampled; the normal map
            // below needs pixels and decodes them from these when it runs.
            prepared = assetManager->loadTextureShared(c, true);
            if (prepared->image.isValid()) {
                resolvedKey = c;
                break;
            }
        }
    }
    if (!prepared || !prepared->image.isValid()) {
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

    const auto& blp = prepared->image;

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
    destroy(device, transparentPipeline_);
    destroy(device, wireframePipeline_);

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/wmo.vert.spv") ||
        !fragShader.loadFromFile(device, (vkCtx_->hardwareRayQueryEnabled() ? "assets/shaders/wmo_hw.frag.spv" : "assets/shaders/wmo.frag.spv"))) {
        core::Logger::getInstance().error("WMORenderer::recreatePipelines: failed to load shaders");
        return;
    }

    buildMainPassPipelines(device, vertShader, fragShader);

    vertShader.destroy();
    fragShader.destroy();

    core::Logger::getInstance().info("WMORenderer: pipelines recreated");
}

void WMORenderer::collectGrassClearings(float minX, float minY, float maxX, float maxY,
                                        std::vector<pipeline::GrassClearingSource>& out) const {
    // How far past a wall the clearing runs, and how far past that the grass
    // takes to stand back up. A building presses harder on its surroundings
    // than a cart does, so these are wider than the M2 numbers.
    constexpr float kClearing = 1.5f;
    constexpr float kEase = 9.0f;
    const float reach = kClearing + kEase;

    for (const auto& inst : scene_->instances) {
        if (inst.worldBoundsMax.x + reach < minX || inst.worldBoundsMin.x - reach > maxX ||
            inst.worldBoundsMax.y + reach < minY || inst.worldBoundsMin.y - reach > maxY) {
            continue;
        }
        // Per-group boxes where they exist; the outer bounds only as the
        // fallback. The outer box of a large WMO swallows its courtyards and
        // half its village, and that is where grass belongs.
        if (!inst.worldGroupBounds.empty()) {
            for (const auto& [mn, mx] : inst.worldGroupBounds) {
                if (mx.x + reach < minX || mn.x - reach > maxX ||
                    mx.y + reach < minY || mn.y - reach > maxY) {
                    continue;
                }
                out.push_back({mn.x, mn.y, mx.x, mx.y, kClearing, kEase});
            }
        } else {
            out.push_back({inst.worldBoundsMin.x, inst.worldBoundsMin.y,
                           inst.worldBoundsMax.x, inst.worldBoundsMax.y, kClearing, kEase});
        }
    }
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
        scene_->rtChanges.modelReady(modelId);
    }
}

void WMORenderer::releaseRtModel(uint32_t modelId) {
    if (!rtScene_) return;
    scene_->rtChanges.modelRemoved(modelId, rtScene_);
    auto mesh = rtModelMeshes_.find(modelId);
    if (mesh == rtModelMeshes_.end()) return;
    rtScene_->removeMesh(mesh->second);
    rtModelMeshes_.erase(mesh);
}

void WMORenderer::syncRtScene() {
    if (!rtScene_) return;
    scene_->rtChanges.flush(*rtScene_, [&](uint32_t id) {
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
