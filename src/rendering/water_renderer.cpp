#include "rendering/water_renderer.hpp"
#include "rendering/sea_bed.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <array>
#include <unordered_map>

namespace wowee {
namespace rendering {

// Matches set 1 binding 0 in water.frag.glsl
struct WaterMaterialUBO {
    glm::vec4 waterColor;
    float waterAlpha;
    float shimmerStrength;
    float alphaScale;
    float bodyFloor;   // least the water's own colour shows, however shallow
};
static_assert(sizeof(WaterMaterialUBO) == 32, "water.frag.glsl's WaterMaterial block is 32 bytes");

// Push constants matching water.vert.glsl
struct WaterPushConstants {
    glm::mat4 model;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType; // 0=water, 1=ocean, 2=magma, 3=slime
    glm::vec2 screenSize;  // target size, for screen-space UVs
    glm::vec2 depthRange;  // camera near, far - the shader linearises SceneDepth with these
    float sceneValid = 0.0f; // 1 once captureSceneHistory has run; the shader used to guess this per pixel
    float meshStep = 0.0f;   // yards between the surface's vertices: the waves the mesh can carry
    float exposed = 0.0f;    // 1 where the wind reaches the surface, 0 indoors where it lies still
    float pad2 = 0.0f;
};
static_assert(sizeof(WaterPushConstants) == 112, "the two shader declarations of this range are laid out for 112 bytes");



WaterRenderer::WaterRenderer() = default;

WaterRenderer::~WaterRenderer() {
    shutdown();
}

bool WaterRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, const SeaBedMap* seaBed) {
    vkCtx = ctx;
    if (!vkCtx) return false;
    seaBed_ = seaBed;
    if (!seaBed_ || seaBed_->view() == VK_NULL_HANDLE) {
        LOG_ERROR("WaterRenderer: no sea bed map to read the water's depth from - no water drawn");
        return false;
    }

    LOG_INFO("Initializing water renderer (Vulkan)");
    VkDevice device = vkCtx->getDevice();

    // --- Material descriptor set layout (set 1) ---
    // binding 0: WaterMaterial UBO
    VkDescriptorSetLayoutBinding matBinding{};
    matBinding.binding = 0;
    matBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    matBinding.descriptorCount = 1;
    matBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialSetLayout = createDescriptorSetLayout(device, { matBinding });
    if (!materialSetLayout) {
        LOG_ERROR("WaterRenderer: failed to create material set layout");
        return false;
    }

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSizes[1]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_WATER_SETS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = MAX_WATER_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &materialDescPool) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create descriptor pool");
        return false;
    }

    // --- Scene history + reflection descriptor set layout (set 2) ---
    VkDescriptorSetLayoutBinding sceneColorBinding{};
    sceneColorBinding.binding = 0;
    sceneColorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneColorBinding.descriptorCount = 1;
    sceneColorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding sceneDepthBinding{};
    sceneDepthBinding.binding = 1;
    sceneDepthBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneDepthBinding.descriptorCount = 1;
    sceneDepthBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding reflColorBinding{};
    reflColorBinding.binding = 2;
    reflColorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    reflColorBinding.descriptorCount = 1;
    reflColorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding reflUBOBinding{};
    reflUBOBinding.binding = 3;
    reflUBOBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    reflUBOBinding.descriptorCount = 1;
    // The wind's waves in it move the vertices as well as light the pixels.
    reflUBOBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    if (vkCtx->isTessellationSupported()) {
        reflUBOBinding.stageFlags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }

    // The sea's surface (OceanWaves): the vertices are carried by it, the
    // pixels lit by its slopes.
    VkDescriptorSetLayoutBinding oceanBinding{};
    oceanBinding.binding = 4;
    oceanBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    oceanBinding.descriptorCount = 1;
    oceanBinding.stageFlags = reflUBOBinding.stageFlags;

    // What lies under the water (SeaBedMap): the waves lie down as it comes
    // up, and are as long as the fetch to the shore upwind lets the wind
    // raise them.
    VkDescriptorSetLayoutBinding seaBedBinding = oceanBinding;
    seaBedBinding.binding = 5;
    VkDescriptorSetLayoutBinding fetchBinding = oceanBinding;
    fetchBinding.binding = 6;
    // What moves through the water leaves in it (WaterWake).
    VkDescriptorSetLayoutBinding wakeBinding = oceanBinding;
    wakeBinding.binding = 7;

    sceneSetLayout = createDescriptorSetLayout(device,
        {sceneColorBinding, sceneDepthBinding, reflColorBinding, reflUBOBinding, oceanBinding, seaBedBinding,
         fetchBinding, wakeBinding});
    if (!sceneSetLayout) {
        LOG_ERROR("WaterRenderer: failed to create scene set layout");
        return false;
    }

    if (!ocean_.initialize(vkCtx)) {
        LOG_ERROR("WaterRenderer: the sea's surface could not be made - no water drawn");
        return false;
    }
    if (!wake_.initialize(vkCtx, seaBed_)) {
        LOG_ERROR("WaterRenderer: the wake could not be made - no water drawn");
        return false;
    }

    // Pool needs 7 combined image samplers + 1 uniform buffer per frame
    std::array<VkDescriptorPoolSize, 2> scenePoolSizes{};
    scenePoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    scenePoolSizes[0].descriptorCount = 7 * SCENE_HISTORY_FRAMES;
    scenePoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    scenePoolSizes[1].descriptorCount = SCENE_HISTORY_FRAMES;
    VkDescriptorPoolCreateInfo scenePoolInfo{};
    scenePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    scenePoolInfo.maxSets = SCENE_HISTORY_FRAMES;
    scenePoolInfo.poolSizeCount = static_cast<uint32_t>(scenePoolSizes.size());
    scenePoolInfo.pPoolSizes = scenePoolSizes.data();
    if (vkCreateDescriptorPool(device, &scenePoolInfo, nullptr, &sceneDescPool) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create scene descriptor pool");
        return false;
    }

    // --- Pipeline layout ---
    pushStages_ = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (vkCtx->isTessellationSupported()) {
        pushStages_ |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = pushStages_;
    pushRange.offset = 0;
    pushRange.size = sizeof(WaterPushConstants);

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, materialSetLayout, sceneSetLayout };
    pipelineLayout = createPipelineLayout(device, setLayouts, { pushRange });
    if (!pipelineLayout) {
        LOG_ERROR("WaterRenderer: failed to create pipeline layout");
        return false;
    }

    // Create reflection resources FIRST so reflectionUBO exists when
    // createSceneHistoryResources writes descriptor binding 3
    createReflectionResources();

    createSceneHistoryResources(refractionCaptureExtent(),
                                vkCtx->getSwapchainFormat(),
                                vkCtx->getDepthFormat());

    waterPipeline = buildWaterPipeline(vkCtx->getImGuiRenderPass(), vkCtx->getMsaaSamples());
    if (!waterPipeline) {
        LOG_ERROR("WaterRenderer: failed to create pipeline");
        return false;
    }

    LOG_INFO("Water renderer initialized (Vulkan)");
    return true;
}

VkPipeline WaterRenderer::buildWaterPipeline(VkRenderPass renderPass, VkSampleCountFlagBits samples) {
    VkDevice device = vkCtx->getDevice();
    // Interleaved: pos3 + normal3 + uv2, 32 bytes; the shaders take the
    // position at 0 and the uv at 1 and work the normal out themselves.
    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding = 0;
    vertBinding.stride = 8 * sizeof(float);
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const std::vector<VkVertexInputAttributeDescription> vertAttribs = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float) },
    };

    // Tessellated where the device can: the waves near the camera need
    // vertices a fraction of a yard apart, and the water's own mesh has them
    // four yards apart (water.tesc, water.tese). Otherwise the water's own
    // mesh, which carries only the longer waves.
    const bool tessellate = vkCtx->isTessellationSupported();
    VkShaderModule vertShader, controlShader, evalShader, fragShader;
    const char* vertPath = tessellate ? "assets/shaders/water_tess.vert.spv"
                                      : "assets/shaders/water.vert.spv";
    if (!vertShader.loadFromFile(device, vertPath) ||
        !fragShader.loadFromFile(device, "assets/shaders/water.frag.spv") ||
        (tessellate && (!controlShader.loadFromFile(device, "assets/shaders/water.tesc.spv") ||
                        !evalShader.loadFromFile(device, "assets/shaders/water.tese.spv")))) {
        LOG_ERROR("WaterRenderer: a water shader did not load (", vertPath,
                  tessellate ? ", water.tesc, water.tese" : "", ", water.frag) - no water drawn");
        vertShader.destroy();
        controlShader.destroy();
        evalShader.destroy();
        fragShader.destroy();
        return VK_NULL_HANDLE;
    }

    PipelineBuilder builder;
    builder.setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                       fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertBinding }, vertAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    if (tessellate) {
        builder.setTessellation(controlShader.stageInfo(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT),
                                evalShader.stageInfo(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT), 3);
    }
    VkPipeline pipeline = builder
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)  // depth test yes, write no
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(samples)
        .setLayout(pipelineLayout)
        .setRenderPass(renderPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());

    vertShader.destroy();
    controlShader.destroy();
    evalShader.destroy();
    fragShader.destroy();
    return pipeline;
}

void WaterRenderer::advanceWindWaves(double deltaSeconds, float windSpeed, glm::vec2 windDirection,
                                     glm::vec2 nearby) {
    ocean_.advance(deltaSeconds, windSpeed, windDirection, nearby);
    for (int c = 0; c < kOceanCascadeCount; ++c) frameUBO_.oceanCascades[c] = ocean_.cascades()[c];
    frameUBO_.oceanSea = ocean_.seaState();
    const auto& kept = ocean_.sea().keptTable();
    std::copy(kept.begin(), kept.end(), frameUBO_.oceanKept);
}

std::optional<float> WaterRenderer::highestOpenWaterNear(glm::vec2 centre, float halfExtent) const {
    std::optional<float> highest;
    for (const DrawnSurface& s : drawn_) {
        if (s.shelter != LiquidShelter::Open || s.basicType > 1) continue;
        const glm::vec3 corner = s.origin + s.stepX * static_cast<float>(s.width) +
                                 s.stepY * static_cast<float>(s.height);
        const glm::vec2 lo = glm::min(glm::vec2(s.origin), glm::vec2(corner));
        const glm::vec2 hi = glm::max(glm::vec2(s.origin), glm::vec2(corner));
        if (hi.x < centre.x - halfExtent || lo.x > centre.x + halfExtent ||
            hi.y < centre.y - halfExtent || lo.y > centre.y + halfExtent) {
            continue;
        }
        if (!highest || s.maxHeight > *highest) highest = s.maxHeight;
    }
    return highest;
}

void WaterRenderer::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    destroyReflectionResources();
    createReflectionResources();
    createSceneHistoryResources(refractionCaptureExtent(),
                                vkCtx->getSwapchainFormat(),
                                vkCtx->getDepthFormat());

    // Destroy old pipeline (keep layout)
    destroy(device, waterPipeline);

    waterPipeline = buildWaterPipeline(vkCtx->getImGuiRenderPass(), vkCtx->getMsaaSamples());
    if (!waterPipeline) {
        LOG_ERROR("WaterRenderer::recreatePipelines: failed to create pipeline");
    }
}

void WaterRenderer::setRefractionEnabled(bool enabled) {
    if (refractionEnabled == enabled) return;
    refractionEnabled = enabled;

    // When turning off, clear scene history images to black so the shader
    // detects "no data" and uses the non-refraction path.
    if (!enabled && vkCtx) {
        vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
            for (auto& sh : sceneHistory) {
                if (!sh.colorImage) continue;

                VkImageMemoryBarrier2 toTransfer{};
                toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toTransfer.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                toTransfer.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.image = sh.colorImage;
                toTransfer.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
                toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                VkDependencyInfo toTransferDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                toTransferDep.imageMemoryBarrierCount = 1;
                toTransferDep.pImageMemoryBarriers = &toTransfer;
                cmdPipelineBarrier2(cmd, toTransferDep);

                VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}};
                VkImageSubresourceRange range = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
                vkCmdClearColorImage(cmd, sh.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

                VkImageMemoryBarrier2 toRead = toTransfer;
                toRead.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                toRead.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                VkDependencyInfo toReadDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                toReadDep.imageMemoryBarrierCount = 1;
                toReadDep.pImageMemoryBarriers = &toRead;
                cmdPipelineBarrier2(cmd, toReadDep);
            }
        });
    }
}

void WaterRenderer::shutdown() {
    destroyAllDrawn();

    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    vkDeviceWaitIdle(device);

    // destroyAllDrawn() defers surface destruction, and those lambdas free descriptor sets
    // from the pools destroyed just below. Drain them here, while the pools are
    // still valid - otherwise they linger until some later subsystem's flush
    // runs them against a dead pool.
    vkCtx->flushDeferredCleanup();

    destroyWater1xResources();
    destroyReflectionResources();
    destroySceneHistoryResources();
    ocean_.shutdown();
    wake_.shutdown();
    destroy(device, waterPipeline);
    destroy(device, pipelineLayout);
    destroy(device, sceneDescPool);
    destroy(device, sceneSetLayout);
    destroy(device, materialDescPool);
    destroy(device, materialSetLayout);

    vkCtx = nullptr;
}

VkDescriptorSet WaterRenderer::allocateMaterialSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx->getDevice(), &allocInfo, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return set;
}

void WaterRenderer::destroySceneHistoryResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    for (auto& sh : sceneHistory) {
        if (sh.colorView) { vkDestroyImageView(device, sh.colorView, nullptr); sh.colorView = VK_NULL_HANDLE; }
        if (sh.depthView) { vkDestroyImageView(device, sh.depthView, nullptr); sh.depthView = VK_NULL_HANDLE; }
        if (sh.colorImage) { vmaDestroyImage(vkCtx->getAllocator(), sh.colorImage, sh.colorAlloc); sh.colorImage = VK_NULL_HANDLE; sh.colorAlloc = VK_NULL_HANDLE; }
        if (sh.depthImage) { vmaDestroyImage(vkCtx->getAllocator(), sh.depthImage, sh.depthAlloc); sh.depthImage = VK_NULL_HANDLE; sh.depthAlloc = VK_NULL_HANDLE; }
        sh.sceneSet = VK_NULL_HANDLE;
    }
    sceneColorSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
    sceneDepthSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
    sceneHistoryExtent = {.width = 0, .height = 0};
    sceneHistoryReady = false;
}

// Full resolution, deliberately.
//
// This was half, on the grounds that refraction is low-frequency - sampled
// through a rippling normal that displaces it by a few pixels anyway - so the
// difference would not show through moving water. That holds for a lake bed.
// It does not hold for the player's own character, which is what is under the
// surface most of the time this is being looked at, and which arrived through
// the refraction visibly aliased however much anti-aliasing was asked for: the
// copy it was sampled from had already thrown half the resolution away, and
// nothing downstream can put that back. It is the thing in the middle of the
// screen, so it gets the pixels.
VkExtent2D WaterRenderer::refractionCaptureExtent() const {
    VkExtent2D full = vkCtx ? vkCtx->getSwapchainExtent() : VkExtent2D{.width = 0, .height = 0};
    return { .width = std::max(1u, full.width), .height = std::max(1u, full.height) };
}

void WaterRenderer::createSceneHistoryResources(VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat) {
    if (!vkCtx || extent.width == 0 || extent.height == 0 || !sceneSetLayout || !sceneDescPool) return;
    VkDevice device = vkCtx->getDevice();

    destroySceneHistoryResources();
    vkResetDescriptorPool(device, sceneDescPool, 0);
    sceneHistoryExtent = extent;

    // Create shared samplers
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sceneColorSampler = vkCtx->getOrCreateSampler(sampCI);
    if (sceneColorSampler == VK_NULL_HANDLE) {
        LOG_ERROR("WaterRenderer: failed to create scene color sampler");
        return;
    }
    sampCI.magFilter = VK_FILTER_NEAREST;
    sampCI.minFilter = VK_FILTER_NEAREST;
    sceneDepthSampler = vkCtx->getOrCreateSampler(sampCI);
    if (sceneDepthSampler == VK_NULL_HANDLE) {
        LOG_ERROR("WaterRenderer: failed to create scene depth sampler");
        return;
    }

    VkImageCreateInfo colorImgInfo{};
    colorImgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    colorImgInfo.imageType = VK_IMAGE_TYPE_2D;
    colorImgInfo.format = colorFormat;
    colorImgInfo.extent = {.width = extent.width, .height = extent.height, .depth = 1};
    colorImgInfo.mipLevels = 1;
    colorImgInfo.arrayLayers = 1;
    colorImgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    colorImgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    colorImgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo depthImgInfo = colorImgInfo;
    depthImgInfo.format = depthFormat;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    // Create per-frame images, views, and descriptor sets
    for (uint32_t f = 0; f < SCENE_HISTORY_FRAMES; f++) {
        auto& sh = sceneHistory[f];

        if (vmaCreateImage(vkCtx->getAllocator(), &colorImgInfo, &allocCI, &sh.colorImage, &sh.colorAlloc, nullptr) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene color history image [", f, "]");
            return;
        }
        if (vmaCreateImage(vkCtx->getAllocator(), &depthImgInfo, &allocCI, &sh.depthImage, &sh.depthAlloc, nullptr) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene depth history image [", f, "]");
            return;
        }

        VkImageViewCreateInfo colorViewInfo{};
        colorViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        colorViewInfo.image = sh.colorImage;
        colorViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        colorViewInfo.format = colorFormat;
        colorViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorViewInfo.subresourceRange.levelCount = 1;
        colorViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &colorViewInfo, nullptr, &sh.colorView) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene color history view [", f, "]");
            return;
        }

        VkImageViewCreateInfo depthViewInfo = colorViewInfo;
        depthViewInfo.image = sh.depthImage;
        depthViewInfo.format = depthFormat;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (vkCreateImageView(device, &depthViewInfo, nullptr, &sh.depthView) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene depth history view [", f, "]");
            return;
        }

        // Allocate descriptor set for this frame
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = sceneDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &sceneSetLayout;
        if (vkAllocateDescriptorSets(device, &ai, &sh.sceneSet) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to allocate scene descriptor set [", f, "]");
            sh.sceneSet = VK_NULL_HANDLE;
            return;
        }

        VkDescriptorImageInfo colorInfo{};
        colorInfo.sampler = sceneColorSampler;
        colorInfo.imageView = sh.colorView;
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = sceneDepthSampler;
        depthInfo.imageView = sh.depthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo reflColorInfo{};
        reflColorInfo.sampler = sceneColorSampler;
        reflColorInfo.imageView = reflectionColorView ? reflectionColorView : sh.colorView;
        reflColorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo reflUBOInfo{};
        reflUBOInfo.buffer = reflectionUBO;
        // Each frame in flight reads its own slot; see uploadFrameUBO.
        reflUBOInfo.offset = static_cast<VkDeviceSize>(f) * kFrameUBOStride;
        reflUBOInfo.range = sizeof(WaterFrameUBOData);

        std::vector<VkWriteDescriptorSet> writes;

        VkWriteDescriptorSet w0{};
        w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w0.dstSet = sh.sceneSet;
        w0.dstBinding = 0;
        w0.descriptorCount = 1;
        w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w0.pImageInfo = &colorInfo;
        writes.push_back(w0);

        VkWriteDescriptorSet w1{};
        w1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w1.dstSet = sh.sceneSet;
        w1.dstBinding = 1;
        w1.descriptorCount = 1;
        w1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w1.pImageInfo = &depthInfo;
        writes.push_back(w1);

        VkWriteDescriptorSet w2{};
        w2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w2.dstSet = sh.sceneSet;
        w2.dstBinding = 2;
        w2.descriptorCount = 1;
        w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w2.pImageInfo = &reflColorInfo;
        writes.push_back(w2);

        if (reflectionUBO) {
            VkWriteDescriptorSet w3{};
            w3.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w3.dstSet = sh.sceneSet;
            w3.dstBinding = 3;
            w3.descriptorCount = 1;
            w3.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w3.pBufferInfo = &reflUBOInfo;
            writes.push_back(w3);
        }

        VkDescriptorImageInfo oceanInfo{};
        oceanInfo.sampler = ocean_.surfaceSampler();
        oceanInfo.imageView = ocean_.surfaceView();
        oceanInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w4{};
        w4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w4.dstSet = sh.sceneSet;
        w4.dstBinding = 4;
        w4.descriptorCount = 1;
        w4.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w4.pImageInfo = &oceanInfo;
        writes.push_back(w4);

        VkDescriptorImageInfo seaBedInfo{};
        seaBedInfo.sampler = seaBed_->sampler();
        seaBedInfo.imageView = seaBed_->view();
        seaBedInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w5 = w4;
        w5.dstBinding = 5;
        w5.pImageInfo = &seaBedInfo;
        writes.push_back(w5);

        VkDescriptorImageInfo fetchInfo = seaBedInfo;
        fetchInfo.imageView = seaBed_->fetchView();
        VkWriteDescriptorSet w6 = w4;
        w6.dstBinding = 6;
        w6.pImageInfo = &fetchInfo;
        writes.push_back(w6);

        VkDescriptorImageInfo wakeInfo{};
        wakeInfo.sampler = wake_.surfaceSampler();
        wakeInfo.imageView = wake_.surfaceView();
        wakeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w7 = w4;
        w7.dstBinding = 7;
        w7.pImageInfo = &wakeInfo;
        writes.push_back(w7);

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Initialize all per-frame history images to shader-read layout
    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        std::vector<VkImageMemoryBarrier2> barriers;
        for (auto& sh : sceneHistory) {
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = sh.colorImage;
            b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers.push_back(b);

            b.image = sh.depthImage;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barriers.push_back(b);
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        cmdPipelineBarrier2(cmd, dep);
    });
}

void WaterRenderer::updateMaterialUBO(DrawnSurface& surface) {
    glm::vec4 color = getLiquidColor(surface.basicType);
    float alpha = getLiquidAlpha(surface.basicType);

    // WMO liquid material override
    if (surface.wmoId != 0) {
        const uint8_t basicType = surface.basicType;
        if (basicType == 2) {
            // Magma - bright orange-red, opaque
            color = glm::vec4(1.0f, 0.35f, 0.05f, 1.0f);
            alpha = 0.95f;
        } else if (basicType == 3) {
            // Slime - green, semi-opaque
            color = glm::vec4(0.2f, 0.6f, 0.1f, 1.0f);
            alpha = 0.85f;
        }
    }

    bool canalProfile = (surface.wmoId != 0) || (surface.liquidType == 5);
    float shimmerStrength = canalProfile ? 0.95f : 0.50f;
    // A canal is a couple of yards deep, and the water's colour is weighted by
    // depth - so it came out as clear glass over the stone bed. Its own colour
    // is given a floor, a little deeper than a lake's, and it is no longer
    // thinned below open water.
    float alphaScale = canalProfile ? 1.15f : 1.00f;
    float bodyFloor = 0.0f;
    if (canalProfile && surface.basicType < 2) {
        color = glm::vec4(0.08f, 0.30f, 0.40f, 1.0f);
        bodyFloor = 0.45f;
    }

    WaterMaterialUBO mat{};
    mat.waterColor = color;
    mat.waterAlpha = alpha;
    mat.shimmerStrength = shimmerStrength;
    mat.alphaScale = alphaScale;
    mat.bodyFloor = bodyFloor;

    // Create UBO
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = sizeof(WaterMaterialUBO);
    bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapInfo{};
    vmaCreateBuffer(vkCtx->getAllocator(), &bufCI, &allocCI,
                    &surface.materialUBO, &surface.materialAlloc, &mapInfo);
    if (mapInfo.pMappedData) {
        std::memcpy(mapInfo.pMappedData, &mat, sizeof(mat));
    }

    // Allocate and write descriptor set
    surface.materialSet = allocateMaterialSet();
    if (surface.materialSet) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = surface.materialUBO;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(WaterMaterialUBO);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = surface.materialSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufInfo;

        vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
    } else {
        LOG_WARNING("Water: failed to allocate material descriptor set (pool exhaustion?)");
    }
}

// ==============================================================
// The scene's surfaces, on the GPU
// ==============================================================

void WaterRenderer::liquidSurfaceAdded(const LiquidSurface& surface) {
    DrawnSurface drawn;
    drawn.id = surface.id;
    drawn.origin = surface.origin;
    drawn.stepX = surface.stepX;
    drawn.stepY = surface.stepY;
    drawn.minHeight = surface.minHeight;
    drawn.maxHeight = surface.maxHeight;
    drawn.liquidType = surface.liquidType;
    drawn.basicType = surface.basicType();
    drawn.wmoId = surface.wmoId;
    drawn.shelter = surface.shelter;
    drawn.width = surface.width;
    drawn.height = surface.height;

    createWaterMesh(drawn, surface);
    // A surface with no cell of liquid has nothing to draw; the scene still
    // knows of it.
    if (drawn.indexCount == 0) return;
    if (vkCtx) updateMaterialUBO(drawn);
    drawn_.push_back(drawn);
}

void WaterRenderer::liquidSurfaceRemoved(LiquidSurfaceId id) {
    for (auto it = drawn_.begin(); it != drawn_.end(); ++it) {
        if (it->id != id) continue;
        destroyWaterMesh(*it);
        drawn_.erase(it);
        return;
    }
}

void WaterRenderer::destroyAllDrawn() {
    for (auto& drawn : drawn_) destroyWaterMesh(drawn);
    drawn_.clear();
}

// ==============================================================
// Rendering
// ==============================================================

void WaterRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                            const Camera& camera, float /*time*/, bool use1x, uint32_t frameIndex) {
    // The sea bed as it was last drawn - this frame, if it was drawn in it.
    const OverheadFrame& bed = seaBed_->frame();
    frameUBO_.seaBedMatrix = bed.viewProjection();
    frameUBO_.seaBed = glm::vec4(bed.ceiling, bed.range, seaBed_->isDrawn() ? 1.0f : 0.0f, 0.0f);
    frameUBO_.wakeArea = wake_.area();    // The wake as it stands, into this frame's slot - safe now, the frame has
    // waited for the GPU to finish with it.
    uploadFrameUBO();
    VkPipeline pipeline = (use1x && water1xPipeline) ? water1xPipeline : waterPipeline;
    if (!renderingEnabled || drawn_.empty() || !pipeline) {
        if (renderDiagCounter_++ % 300 == 0 && !drawn_.empty()) {
            LOG_WARNING("Water: render skipped - enabled=", renderingEnabled,
                        " surfaces=", drawn_.size(),
                        " pipeline=", (pipeline ? "ok" : "null"),
                        " use1x=", use1x);
        }
        return;
    }
    uint32_t fi = frameIndex % SCENE_HISTORY_FRAMES;
    VkDescriptorSet activeSceneSet = sceneHistory[fi].sceneSet;
    if (!activeSceneSet) {
        if (renderDiagCounter_++ % 300 == 0) {
            LOG_WARNING("Water: render skipped - sceneSet is null, surfaces=", drawn_.size());
        }
        return;
    }

    // Frustum culling setup
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                             0, 1, &perFrameSet, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                             2, 1, &activeSceneSet, 0, nullptr);

    for (const auto& surface : drawn_) {
        if (surface.vertexBuffer == VK_NULL_HANDLE || surface.indexCount == 0) continue;
        if (!surface.materialSet) continue;

        // Frustum cull: compute AABB from surface origin + step vectors
        {
            const glm::vec3 extentX = surface.stepX * static_cast<float>(surface.width);
            const glm::vec3 extentY = surface.stepY * static_cast<float>(surface.height);
            const glm::vec3 c0 = surface.origin;
            const glm::vec3 c1 = surface.origin + extentX;
            const glm::vec3 c2 = surface.origin + extentY;
            const glm::vec3 c3 = surface.origin + extentX + extentY;
            const glm::vec3 aabbMin(
                std::min({c0.x, c1.x, c2.x, c3.x}),
                std::min({c0.y, c1.y, c2.y, c3.y}),
                surface.minHeight
            );
            const glm::vec3 aabbMax(
                std::max({c0.x, c1.x, c2.x, c3.x}),
                std::max({c0.y, c1.y, c2.y, c3.y}),
                surface.maxHeight
            );
            if (!frustum.intersectsAABB(aabbMin, aabbMax)) continue;
        }

        bool isWmoWater = (surface.wmoId != 0);
        bool canalProfile = isWmoWater || (surface.liquidType == 5);
        const uint8_t basicType = surface.basicType;
        // WMO water gets no wave displacement - prevents visible slosh at
        // geometry edges (bridges, docks) where water is far below the surface.
        float waveAmp = isWmoWater ? 0.0f : (basicType == 1 ? 0.35f : 0.08f);
        float waveFreq = canalProfile ? 0.35f : (basicType == 1 ? 0.20f : 0.30f);
        float waveSpeed = canalProfile ? 1.00f : (basicType == 1 ? 1.20f : 1.40f);

        WaterPushConstants push{};
        push.model = glm::mat4(1.0f);
        push.waveAmp = waveAmp;
        push.waveFreq = waveFreq;
        push.waveSpeed = waveSpeed;
        push.liquidBasicType = static_cast<float>(basicType);
        push.screenSize = glm::vec2(static_cast<float>(renderExtent_.width),
                                    static_cast<float>(renderExtent_.height));
        // Asked of the camera rather than written out in the shader, which is
        // how the two came to disagree: the shader linearised the depth buffer
        // against a near plane of 0.05 while the camera's is 0.5.
        push.depthRange = glm::vec2(camera.getNearPlane(), camera.getFarPlane());
        push.sceneValid = sceneHistoryReady ? 1.0f : 0.0f;
        push.meshStep = std::max(glm::length(surface.stepX), glm::length(surface.stepY));
        // A building's water outdoors takes the wind as the sea does; where
        // it meets its quays, piers and walls the sea bed's map has no depth
        // under it and its waves lie down there. Indoors it lies still.
        push.exposed = surface.shelter == LiquidShelter::Open ? 1.0f : 0.0f;

        vkCmdPushConstants(cmd, pipelineLayout, pushStages_,
                            0, sizeof(WaterPushConstants), &push);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                 1, 1, &surface.materialSet, 0, nullptr);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &surface.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, surface.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(surface.indexCount), 1, 0, 0, 0);
    }
}

void WaterRenderer::captureSceneHistory(VkCommandBuffer cmd,
                                        VkImage srcColorImage,
                                        VkImage srcDepthImage,
                                        VkExtent2D srcExtent,
                                        bool srcDepthIsMsaa,
                                        uint32_t frameIndex) {
    uint32_t fi = frameIndex % SCENE_HISTORY_FRAMES;
    auto& sh = sceneHistory[fi];
    if (!vkCtx || !cmd || !sh.colorImage || !sh.depthImage || srcExtent.width == 0 || srcExtent.height == 0) {
        return;
    }

    // The scene is not always rendered at the history's resolution - FSR renders
    // smaller and upscales later. Copying the smaller image into the corner of a
    // larger history would leave the refraction sampling a fraction of the frame
    // stretched over all of it, so scale with a blit whenever they differ and
    // keep the cheap straight copy when they match.
    const bool needsScaling = (srcExtent.width != sceneHistoryExtent.width ||
                               srcExtent.height != sceneHistoryExtent.height);
    VkExtent2D copyExtent{
        .width = std::min(srcExtent.width, sceneHistoryExtent.width),
        .height = std::min(srcExtent.height, sceneHistoryExtent.height)
    };
    if (copyExtent.width == 0 || copyExtent.height == 0) return;

    auto barrier2 = [&](VkImage image,
                        VkImageAspectFlags aspect,
                        VkImageLayout oldLayout,
                        VkImageLayout newLayout,
                        VkAccessFlags srcAccess,
                        VkAccessFlags dstAccess,
                        VkPipelineStageFlags srcStage,
                        VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage;
        b.dstStageMask = dstStage;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };

    // Color source: final render pass layout is PRESENT_SRC.
    // srcAccessMask must be COLOR_ATTACHMENT_WRITE (not 0) so that GPU cache flushes
    // happen before the transfer read.  Using srcAccessMask=0 with BOTTOM_OF_PIPE
    // causes VK_ERROR_DEVICE_LOST on strict drivers (AMD/Mali) because color writes
    // are not made visible to the transfer unit before the copy begins.
    barrier2(srcColorImage, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier2(sh.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (needsScaling) {
        VkImageBlit colorBlit{};
        colorBlit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        colorBlit.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        colorBlit.srcOffsets[1] = {.x = static_cast<int32_t>(srcExtent.width),
                                   .y = static_cast<int32_t>(srcExtent.height), .z = 1};
        colorBlit.dstOffsets[1] = {.x = static_cast<int32_t>(sceneHistoryExtent.width),
                                   .y = static_cast<int32_t>(sceneHistoryExtent.height), .z = 1};
        vkCmdBlitImage(cmd, srcColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sh.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &colorBlit, VK_FILTER_LINEAR);
    } else {
        VkImageCopy colorCopy{};
        colorCopy.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        colorCopy.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        colorCopy.extent = {.width = copyExtent.width, .height = copyExtent.height, .depth = 1};
        vkCmdCopyImage(cmd, srcColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sh.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorCopy);
    }

    barrier2(sh.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier2(srcColorImage, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
             VK_ACCESS_TRANSFER_READ_BIT, 0,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    // Depth source: only copy when source is single-sampled.
    if (!srcDepthIsMsaa && srcDepthImage != VK_NULL_HANDLE) {
        barrier2(srcDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier2(sh.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        if (needsScaling) {
            // Depth must not be filtered - an interpolated depth is a surface
            // that exists nowhere.
            VkImageBlit depthBlit{};
            depthBlit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthBlit.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthBlit.srcOffsets[1] = {.x = static_cast<int32_t>(srcExtent.width),
                                       .y = static_cast<int32_t>(srcExtent.height), .z = 1};
            depthBlit.dstOffsets[1] = {.x = static_cast<int32_t>(sceneHistoryExtent.width),
                                       .y = static_cast<int32_t>(sceneHistoryExtent.height), .z = 1};
            vkCmdBlitImage(cmd, srcDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           sh.depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &depthBlit, VK_FILTER_NEAREST);
        } else {
            VkImageCopy depthCopy{};
            depthCopy.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthCopy.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthCopy.extent = {.width = copyExtent.width, .height = copyExtent.height, .depth = 1};
            vkCmdCopyImage(cmd, srcDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           sh.depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);
        }

        barrier2(sh.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        barrier2(srcDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    }

    sceneHistoryReady = true;
}

// ==============================================================
// Mesh creation (Vulkan upload instead of GL)
// ==============================================================

void WaterRenderer::createWaterMesh(DrawnSurface& drawn, const LiquidSurface& surface) {
    const int gridWidth = surface.width + 1;
    const int gridHeight = surface.height + 1;
    constexpr float VISUAL_WATER_Z_BIAS = 0.02f;

    std::vector<float> vertices;
    std::vector<uint32_t> indices;

    for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
            int index = y * gridWidth + x;
            float height = (index < static_cast<int>(surface.heights.size()))
                ? surface.heights[index] : surface.minHeight;

            glm::vec3 pos = surface.origin +
                            surface.stepX * static_cast<float>(x) +
                            surface.stepY * static_cast<float>(y);
            pos.z = height + VISUAL_WATER_Z_BIAS;

            // pos (3 floats)
            vertices.push_back(pos.x);
            vertices.push_back(pos.y);
            vertices.push_back(pos.z);
            // normal (3 floats) - up
            vertices.push_back(0.0f);
            vertices.push_back(0.0f);
            vertices.push_back(1.0f);
            // texcoord (2 floats)
            vertices.push_back(static_cast<float>(x) / std::max(1, gridWidth - 1));
            vertices.push_back(static_cast<float>(y) / std::max(1, gridHeight - 1));
        }
    }

    // Generate indices respecting render mask (same logic as GL version)
    for (int y = 0; y < gridHeight - 1; y++) {
        for (int x = 0; x < gridWidth - 1; x++) {
            if (!surface.cellHasLiquid(x, y)) continue;

            int topLeft = y * gridWidth + x;
            int topRight = topLeft + 1;
            int bottomLeft = (y + 1) * gridWidth + x;
            int bottomRight = bottomLeft + 1;

            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    if (indices.empty()) return;
    drawn.indexCount = static_cast<int>(indices.size());

    if (!vkCtx) return;

    // Upload vertex buffer
    VkDeviceSize vbSize = vertices.size() * sizeof(float);
    AllocatedBuffer vb = uploadBuffer(*vkCtx, vertices.data(), vbSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    drawn.vertexBuffer = vb.buffer;
    drawn.vertexAlloc = vb.allocation;

    // Upload index buffer
    VkDeviceSize ibSize = indices.size() * sizeof(uint32_t);
    AllocatedBuffer ib = uploadBuffer(*vkCtx, indices.data(), ibSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    drawn.indexBuffer = ib.buffer;
    drawn.indexAlloc = ib.allocation;
}

void WaterRenderer::destroyWaterMesh(DrawnSurface& surface) {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    ::VkBuffer vertexBuffer = surface.vertexBuffer;
    VmaAllocation vertexAlloc = surface.vertexAlloc;
    ::VkBuffer indexBuffer = surface.indexBuffer;
    VmaAllocation indexAlloc = surface.indexAlloc;
    ::VkBuffer materialUBO = surface.materialUBO;
    VmaAllocation materialAlloc = surface.materialAlloc;
    VkDescriptorPool pool = materialDescPool;
    VkDescriptorSet materialSet = surface.materialSet;

    surface.vertexBuffer = VK_NULL_HANDLE;
    surface.vertexAlloc = VK_NULL_HANDLE;
    surface.indexBuffer = VK_NULL_HANDLE;
    surface.indexAlloc = VK_NULL_HANDLE;
    surface.materialUBO = VK_NULL_HANDLE;
    surface.materialAlloc = VK_NULL_HANDLE;
    surface.materialSet = VK_NULL_HANDLE;

    vkCtx->deferAfterAllFrameFences([device, allocator, vertexBuffer, vertexAlloc, indexBuffer, indexAlloc,
                                     materialUBO, materialAlloc, pool, materialSet]() {
        if (vertexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = vertexBuffer; ab.allocation = vertexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (indexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = indexBuffer; ab.allocation = indexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (materialUBO) {
            AllocatedBuffer ab{}; ab.buffer = materialUBO; ab.allocation = materialAlloc;
            destroyBuffer(allocator, ab);
        }
        if (materialSet && pool) {
            VkDescriptorSet set = materialSet;
            vkFreeDescriptorSets(device, pool, 1, &set);
        }
    });
}

glm::vec4 WaterRenderer::getLiquidColor(uint8_t basicType) const {
    switch (basicType) {
        case 0:  return glm::vec4(0.10f, 0.28f, 0.55f, 1.0f); // inland: richer blue
        case 1:  return glm::vec4(0.04f, 0.16f, 0.38f, 1.0f); // ocean: deep blue
        case 2:  return glm::vec4(0.9f, 0.3f, 0.05f, 1.0f);   // magma
        case 3:  return glm::vec4(0.2f, 0.6f, 0.1f, 1.0f);    // slime
        default: return glm::vec4(0.10f, 0.28f, 0.55f, 1.0f);
    }
}

float WaterRenderer::getLiquidAlpha(uint8_t basicType) const {
    switch (basicType) {
        case 1:  return 0.72f;  // ocean
        case 2:  return 0.75f;  // magma
        case 3:  return 0.65f;  // slime
        default: return 0.48f;  // inland water
    }
}

// ==============================================================
// Planar reflection resources
// ==============================================================

void WaterRenderer::createReflectionResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    // --- Reflection color image ---
    VkImageCreateInfo colorImgCI{};
    colorImgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    colorImgCI.imageType = VK_IMAGE_TYPE_2D;
    colorImgCI.format = vkCtx->getSwapchainFormat();
    colorImgCI.extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT, .depth = 1};
    colorImgCI.mipLevels = 1;
    colorImgCI.arrayLayers = 1;
    colorImgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    colorImgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImgCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    colorImgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator, &colorImgCI, &allocCI,
                       &reflectionColorImage, &reflectionColorAlloc, nullptr) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection color image");
        return;
    }

    VkImageViewCreateInfo colorViewCI{};
    colorViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    colorViewCI.image = reflectionColorImage;
    colorViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    colorViewCI.format = vkCtx->getSwapchainFormat();
    colorViewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    if (vkCreateImageView(device, &colorViewCI, nullptr, &reflectionColorView) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection color view");
        return;
    }

    // --- Reflection depth image ---
    VkImageCreateInfo depthImgCI{};
    depthImgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImgCI.imageType = VK_IMAGE_TYPE_2D;
    depthImgCI.format = vkCtx->getDepthFormat();
    depthImgCI.extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT, .depth = 1};
    depthImgCI.mipLevels = 1;
    depthImgCI.arrayLayers = 1;
    depthImgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vmaCreateImage(allocator, &depthImgCI, &allocCI,
                       &reflectionDepthImage, &reflectionDepthAlloc, nullptr) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection depth image");
        return;
    }

    VkImageViewCreateInfo depthViewCI{};
    depthViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewCI.image = reflectionDepthImage;
    depthViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewCI.format = vkCtx->getDepthFormat();
    depthViewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    if (vkCreateImageView(device, &depthViewCI, nullptr, &reflectionDepthView) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection depth view");
        return;
    }

    // --- Reflection sampler ---
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    reflectionSampler = vkCtx->getOrCreateSampler(sampCI);
    if (reflectionSampler == VK_NULL_HANDLE) {
        LOG_ERROR("WaterRenderer: failed to create reflection sampler");
        return;
    }

    // --- Reflection render pass ---
    VkAttachmentDescription colorAttach{};
    colorAttach.format = vkCtx->getSwapchainFormat();
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttach{};
    depthAttach.format = vkCtx->getDepthFormat();
    depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{.attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttach, depthAttach};
    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpCI.pAttachments = attachments.data();
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;

    if (vkCreateRenderPass(device, &rpCI, nullptr, &reflectionRenderPass) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection render pass");
        return;
    }

    // --- Reflection framebuffer ---
    std::array<VkImageView, 2> fbAttach = {reflectionColorView, reflectionDepthView};
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = reflectionRenderPass;
    fbCI.attachmentCount = static_cast<uint32_t>(fbAttach.size());
    fbCI.pAttachments = fbAttach.data();
    fbCI.width = REFLECTION_WIDTH;
    fbCI.height = REFLECTION_HEIGHT;
    fbCI.layers = 1;

    if (vkCreateFramebuffer(device, &fbCI, nullptr, &reflectionFramebuffer) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection framebuffer");
        return;
    }

    // --- Reflection UBO ---
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = kFrameUBOStride * SCENE_HISTORY_FRAMES;
    bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo uboAllocCI{};
    uboAllocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    uboAllocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapInfo{};
    if (vmaCreateBuffer(allocator, &bufCI, &uboAllocCI,
                        &reflectionUBO, &reflectionUBOAlloc, &mapInfo) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection UBO");
        return;
    }
    reflectionUBOMapped = mapInfo.pMappedData;

    // Initialize with identity
    WaterFrameUBOData initData{};
    initData.reflViewProj = glm::mat4(1.0f);
    if (reflectionUBOMapped) {
        for (uint32_t f = 0; f < SCENE_HISTORY_FRAMES; ++f) {
            std::memcpy(static_cast<char*>(reflectionUBOMapped) + f * kFrameUBOStride,
                        &initData, sizeof(initData));
        }
    }

    // Transition reflection color image to shader-read so first frame doesn't read undefined
    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = reflectionColorImage;
        barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        cmdPipelineBarrier2(cmd, dep);
    });
    reflectionColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    LOG_INFO("Water reflection resources created (", REFLECTION_WIDTH, "x", REFLECTION_HEIGHT, ")");
}

void WaterRenderer::destroyReflectionResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    if (reflectionFramebuffer) { vkDestroyFramebuffer(device, reflectionFramebuffer, nullptr); reflectionFramebuffer = VK_NULL_HANDLE; }
    if (reflectionRenderPass) { vkDestroyRenderPass(device, reflectionRenderPass, nullptr); reflectionRenderPass = VK_NULL_HANDLE; }
    if (reflectionColorView) { vkDestroyImageView(device, reflectionColorView, nullptr); reflectionColorView = VK_NULL_HANDLE; }
    if (reflectionDepthView) { vkDestroyImageView(device, reflectionDepthView, nullptr); reflectionDepthView = VK_NULL_HANDLE; }
    if (reflectionColorImage) { vmaDestroyImage(allocator, reflectionColorImage, reflectionColorAlloc); reflectionColorImage = VK_NULL_HANDLE; }
    if (reflectionDepthImage) { vmaDestroyImage(allocator, reflectionDepthImage, reflectionDepthAlloc); reflectionDepthImage = VK_NULL_HANDLE; }
    reflectionSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
    if (reflectionUBO) {
        AllocatedBuffer ab{}; ab.buffer = reflectionUBO; ab.allocation = reflectionUBOAlloc;
        destroyBuffer(allocator, ab);
        reflectionUBO = VK_NULL_HANDLE;
        reflectionUBOMapped = nullptr;
    }
    reflectionColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// ==============================================================
// Reflection pass begin/end
// ==============================================================

bool WaterRenderer::beginReflectionPass(VkCommandBuffer cmd) {
    if (!reflectionRenderPass || !reflectionFramebuffer || !cmd) return false;

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = reflectionRenderPass;
    rpInfo.framebuffer = reflectionFramebuffer;
    rpInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}};

    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {.depth = 1.0f, .stencil = 0};
    rpInfo.clearValueCount = 2;
    rpInfo.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{.x = 0, .y = 0, .width = static_cast<float>(REFLECTION_WIDTH), .height = static_cast<float>(REFLECTION_HEIGHT), .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{.offset = {.x = 0, .y = 0}, .extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    return true;
}

void WaterRenderer::endReflectionPass(VkCommandBuffer cmd) {
    if (!cmd) return;
    vkCmdEndRenderPass(cmd);
    reflectionColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Update only the current frame's scene descriptor set with the reflection texture.
    // Updating all frames would race with in-flight command buffers that have the
    // other frame's descriptor set bound.
    if (reflectionColorView && reflectionSampler && vkCtx) {
        uint32_t fi = vkCtx->getCurrentFrame() % SCENE_HISTORY_FRAMES;
        if (sceneHistory[fi].sceneSet) {
            VkDescriptorImageInfo reflInfo{};
            reflInfo.sampler = reflectionSampler;
            reflInfo.imageView = reflectionColorView;
            reflInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = sceneHistory[fi].sceneSet;
            write.dstBinding = 2;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &reflInfo;
            vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
        }
    }
}

void WaterRenderer::updateReflectionUBO(const glm::mat4& reflViewProj) {
    frameUBO_.reflViewProj = reflViewProj;
    uploadFrameUBO();
}

// Into the slot of the frame being recorded. There was one slot for every frame
// in flight, so the reflection matrix was rewritten for the next frame while the
// GPU was still drawing water with it for the last one - the reflection was
// sampled through a camera a frame out of step with the one that rendered it,
// and shook whenever the view moved.
void WaterRenderer::uploadFrameUBO() {
    if (!reflectionUBOMapped || !vkCtx) return;
    const uint32_t slot = vkCtx->getCurrentFrame() % SCENE_HISTORY_FRAMES;
    std::memcpy(static_cast<char*>(reflectionUBOMapped) + slot * kFrameUBOStride,
                &frameUBO_, sizeof(frameUBO_));
}

// Disturbance trail. Points are dropped along the path rather than parented to
// the character, so the froth stays where the water was churned and the
// character runs out of it - froth pinned under the feet reads as a decal.
void WaterRenderer::updateWake(float deltaTime, const glm::vec2& pos,
                               const glm::vec2& travelDir, float intensity, bool wading) {
    // Age the existing trail. Drift is what spreads a swimmer's two emission
    // lines apart with distance behind, which is the V.
    for (auto& p : wakePoints_) {
        p.age += deltaTime;
        p.pos += p.drift * deltaTime;
        // Barely any drag. A wake's arms are straight because the separation
        // keeps growing with distance behind; damping the drift collapsed the
        // two arms back onto each other and the V read as a single line.
        p.drift *= std::max(0.0f, 1.0f - 0.25f * deltaTime);
    }
    wakePoints_.erase(std::remove_if(wakePoints_.begin(), wakePoints_.end(),
                                     [](const WakePoint& p) { return p.age >= p.life; }),
                      wakePoints_.end());

    if (intensity > 0.0f) {
        // Emit by distance travelled, not by time: standing still in the
        // shallows should settle rather than pile froth up in one spot.
        // Close enough together that the patches overlap into a continuous
        // trail. Spaced out, each one reads as its own blob.
        const float spacing = wading ? 0.40f : 0.60f;
        bool emit = !hasWakeEmitPos_;
        if (!emit) {
            const glm::vec2 delta = pos - lastWakeEmitPos_;
            emit = glm::dot(delta, delta) >= spacing * spacing;
        }

        if (emit) {
            hasWakeEmitPos_ = true;
            lastWakeEmitPos_ = pos;

            glm::vec2 fwd = travelDir;
            const float fwdLenSq = glm::dot(fwd, fwd);
            fwd = (fwdLenSq > 1e-6f) ? fwd * glm::inversesqrt(fwdLenSq) : glm::vec2(0.0f, 1.0f);
            const glm::vec2 side(-fwd.y, fwd.x);

            auto push = [&](const glm::vec2& at, const glm::vec2& drift,
                            float life, float strength) {
                if (static_cast<int>(wakePoints_.size()) >= kMaxWakePoints) {
                    wakePoints_.erase(wakePoints_.begin());  // oldest goes first
                }
                WakePoint wp;
                wp.pos = at;
                wp.drift = drift;
                wp.life = life;
                wp.strength = strength;
                wakePoints_.push_back(wp);
            };

            if (wading) {
                // Legs punching through the surface: one churned patch per
                // stride, thrown slightly forward of where the foot lands.
                push(pos + fwd * 0.25f, side * 0.30f, 1.5f, intensity);
            } else {
                // Swimming: a pair off the shoulders, each drifting outward and
                // back. The arms open with age because the emissions keep
                // separating after the swimmer has moved on, so the trail is a V
                // even though every point is laid down on the same line.
                const float spread = 0.50f;
                const float lateral = 1.15f;
                const float life = 1.9f;
                push(pos + side * spread - fwd * 0.2f,
                     side * lateral - fwd * 0.30f, life, intensity * 0.8f);
                push(pos - side * spread - fwd * 0.2f,
                     -side * lateral - fwd * 0.30f, life, intensity * 0.8f);
            }
        }
    } else {
        hasWakeEmitPos_ = false;
    }

    // Pack for the shader, and build the bounding circle the fragment shader
    // uses to reject the rest of the sheet in one test.
    const int count = std::min(static_cast<int>(wakePoints_.size()), kMaxWakePoints);
    glm::vec2 centre(0.0f);
    for (int i = 0; i < count; ++i) centre += wakePoints_[i].pos;
    if (count > 0) centre /= static_cast<float>(count);

    float cullRadius = 0.0f;
    for (int i = 0; i < count; ++i) {
        const WakePoint& p = wakePoints_[i];
        const float age01 = std::min(1.0f, p.age / p.life);
        // Must cover the point's own spread, or froth is clipped mid-patch.
        constexpr float kMaxPatchRadius = 1.45f;
        cullRadius = std::max(cullRadius, glm::length(p.pos - centre) + kMaxPatchRadius);
        frameUBO_.wakePoints[i] = glm::vec4(p.pos.x, p.pos.y, age01, p.strength);
    }
    frameUBO_.wakeBounds = glm::vec4(centre.x, centre.y, cullRadius, static_cast<float>(count));
    // Uploaded when the frame is recorded, not here: this runs before the frame
    // has waited for its slot, and the slot it would name is still being read
    // by the frame the GPU is drawing.
    
}


// ==============================================================
// Separate 1x water pass (used when MSAA is active)
// ==============================================================

bool WaterRenderer::createWater1xPass(VkFormat colorFormat, VkFormat depthFormat) {
    if (!vkCtx) return false;
    VkDevice device = vkCtx->getDevice();

    VkAttachmentDescription attachments[2]{};
    // Color: load existing resolved content, store after water draw
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth: load resolved depth for depth testing
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{.attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 2;
    rpCI.pAttachments = attachments;
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;

    if (vkCreateRenderPass(device, &rpCI, nullptr, &water1xRenderPass) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create 1x water render pass");
        return false;
    }

    // The same water, drawn against this pass
    water1xPipeline = buildWaterPipeline(water1xRenderPass, VK_SAMPLE_COUNT_1_BIT);
    if (!water1xPipeline) {
        LOG_ERROR("WaterRenderer: failed to create 1x water pipeline");
        return false;
    }

    LOG_INFO("WaterRenderer: created 1x water pass and pipeline");
    return true;
}

void WaterRenderer::createWater1xFramebuffers(const std::vector<VkImageView>& swapViews,
                                               VkImageView depthView, VkExtent2D extent) {
    if (!vkCtx || !water1xRenderPass || !depthView) return;
    VkDevice device = vkCtx->getDevice();

    // Destroy old framebuffers
    for (auto fb : water1xFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    water1xFramebuffers.clear();

    water1xFramebuffers.resize(swapViews.size());
    for (size_t i = 0; i < swapViews.size(); i++) {
        VkImageView views[2] = { swapViews[i], depthView };
        VkFramebufferCreateInfo fbCI{};
        fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbCI.renderPass = water1xRenderPass;
        fbCI.attachmentCount = 2;
        fbCI.pAttachments = views;
        fbCI.width = extent.width;
        fbCI.height = extent.height;
        fbCI.layers = 1;
        if (vkCreateFramebuffer(device, &fbCI, nullptr, &water1xFramebuffers[i]) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create 1x framebuffer ", i);
        }
    }
}

void WaterRenderer::destroyWater1xResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    for (auto fb : water1xFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    water1xFramebuffers.clear();
    destroy(device, water1xPipeline);
    if (water1xRenderPass) { vkDestroyRenderPass(device, water1xRenderPass, nullptr); water1xRenderPass = VK_NULL_HANDLE; }
}
} // namespace rendering
} // namespace wowee
