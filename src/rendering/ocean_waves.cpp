#include "rendering/ocean_waves.hpp"
#include "rendering/glsl_as_cpp.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::rendering::glsl {
namespace {
// The shaders' whole include, of which this takes the grid and the patches.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../assets/shaders/ocean_spectrum.glsli"
#pragma GCC diagnostic pop
}
}  // namespace wowee::rendering::glsl

namespace wowee {
namespace rendering {

namespace {

static_assert(kOceanCascadeCount == glsl::kOceanCascades, "the shaders and the renderer draw as many patches");

constexpr uint32_t kSize = static_cast<uint32_t>(glsl::kOceanSize);
constexpr uint32_t kCascades = static_cast<uint32_t>(kOceanCascadeCount);
constexpr uint32_t kGroup = 16;  // the 16 x 16 passes: spectrum, evolve, assemble

constexpr VkFormat kStateFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kModesFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kSurfaceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

/// Must match OceanPush in ocean_waves.glsli.
struct OceanPushGPU {
    glm::vec2 toward;
    float speed;
    float slopeScale;
    float deltaTime;
    float time;
    float ride;
    int32_t restart;
    int32_t vertical;
};

bool hasFeatures(VkPhysicalDevice device, VkFormat format, VkFormatFeatureFlags needed) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    return (props.optimalTilingFeatures & needed) == needed;
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStages;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStages;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    cmdPipelineBarrier2(cmd, dep);
}

}  // namespace

OceanWaves::~OceanWaves() {
    shutdown();
}

bool OceanWaves::initialize(VkContext* ctx) {
    if (!ctx) return false;
    ctx_ = ctx;
    VkPhysicalDevice physical = ctx_->getPhysicalDevice();

    // The surface is sampled by the water whatever else happens, so it is
    // made first and flat; a device that cannot build the sea gets still
    // water, said once here.
    surfaceMips_ = static_cast<uint32_t>(std::log2(static_cast<float>(kSize))) + 1;
    const bool surfaceFeatures = hasFeatures(physical, kSurfaceFormat,
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (!createImage(surface_, kSurfaceFormat, 3 * kCascades, surfaceMips_,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     "ocean surface")) {
        LOG_ERROR("OceanWaves: could not create the ", kSize, "x", kSize, "x", 3 * kCascades,
                  " surface image - no water drawn");
        return false;
    }
    // The view the water samples: every layer and every mip.
    VkImageViewCreateInfo sampledCI{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    sampledCI.image = surface_.image;
    sampledCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    sampledCI.format = kSurfaceFormat;
    sampledCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                  .levelCount = surfaceMips_, .baseArrayLayer = 0, .layerCount = 3 * kCascades};
    if (vkCreateImageView(ctx_->getDevice(), &sampledCI, nullptr, &surfaceSampledView_) != VK_SUCCESS) {
        LOG_ERROR("OceanWaves: vkCreateImageView failed for the surface's mips - no water drawn");
        return false;
    }

    VkSamplerCreateInfo sampCI{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // The water is seen at a glancing angle across most of the view, where
    // a pixel covers a long, thin strip of the sea.
    sampCI.anisotropyEnable = VK_TRUE;
    sampCI.maxAnisotropy = 8.0f;
    sampCI.maxLod = VK_LOD_CLAMP_NONE;
    sampler_ = ctx_->getOrCreateSampler(sampCI);
    if (sampler_ == VK_NULL_HANDLE) {
        LOG_ERROR("OceanWaves: failed to create the surface sampler - no water drawn");
        return false;
    }

    if (!surfaceFeatures ||
        !hasFeatures(physical, kModesFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !hasFeatures(physical, kStateFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        LOG_WARNING("OceanWaves: this device cannot store or filter the sea's images - the water lies still");
        return true;
    }
    if (!createImage(state_, kStateFormat, kCascades, 1, VK_IMAGE_USAGE_STORAGE_BIT, "ocean spectrum") ||
        !createImage(modes_, kModesFormat, 2 * kCascades, 1, VK_IMAGE_USAGE_STORAGE_BIT, "ocean modes") ||
        !createPasses()) {
        LOG_WARNING("OceanWaves: the sea's compute passes did not build - the water lies still");
        return true;
    }
    passesReady_ = true;
    LOG_INFO("OceanWaves: ", kCascades, " cascades of ", kSize, "x", kSize, " waves");
    return true;
}

bool OceanWaves::createImage(Image& out, VkFormat format, uint32_t layers, uint32_t mips,
                             VkImageUsageFlags usage, const char* name) {
    VkImageCreateInfo imgCI{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = format;
    imgCI.extent = {.width = kSize, .height = kSize, .depth = 1};
    imgCI.mipLevels = mips;
    imgCI.arrayLayers = layers;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (gpuCreateImage(ctx_->getAllocator(), &imgCI, &allocCI, &out.image, &out.alloc, nullptr) != VK_SUCCESS) {
        LOG_ERROR("OceanWaves: vmaCreateImage failed for the ", name, " image");
        return false;
    }
    setObjectName(ctx_->getDevice(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(out.image), name);

    // The view the passes write: mip 0, every layer.
    VkImageViewCreateInfo viewCI{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCI.image = out.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewCI.format = format;
    viewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                               .baseArrayLayer = 0, .layerCount = layers};
    if (vkCreateImageView(ctx_->getDevice(), &viewCI, nullptr, &out.view) != VK_SUCCESS) {
        LOG_ERROR("OceanWaves: vkCreateImageView failed for the ", name, " image");
        destroyImage(out);
        return false;
    }

    // To GENERAL for good, written as storage, blitted and sampled there,
    // and cleared to still water: no variance, no waves.
    const VkImageSubresourceRange all{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                      .levelCount = mips, .baseArrayLayer = 0, .layerCount = layers};
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toGeneral{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = out.image;
        toGeneral.subresourceRange = all;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toGeneral;
        cmdPipelineBarrier2(cmd, dep);

        const VkClearColorValue still{};
        vkCmdClearColorImage(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, &still, 1, &all);

        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    });
    return true;
}

void OceanWaves::destroyImage(Image& image) {
    if (!ctx_) return;
    DeferredResources retired(*ctx_);
    retired.release(image.view, vkDestroyImageView);
    retired.image(image.image, image.alloc);
}

bool OceanWaves::createPasses() {
    VkDevice device = ctx_->getDevice();

    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = 3;
    layoutCI.pBindings = b;
    if (vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &setLayout_) != VK_SUCCESS) {
        LOG_ERROR("OceanWaves: failed to create the descriptor set layout");
        return false;
    }

    VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(OceanPushGPU)};
    VkPipelineLayoutCreateInfo plCI{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &setLayout_;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &plCI, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("OceanWaves: failed to create the pipeline layout");
        return false;
    }

    const auto makePipeline = [&](const char* path, VkPipeline& out) {
        VkShaderModule module;
        if (!module.loadFromFile(device, path)) {
            LOG_ERROR("OceanWaves: failed to load ", path);
            return false;
        }
        VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = pipelineLayout_;
        const bool ok = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI,
                                                 nullptr, &out) == VK_SUCCESS;
        module.destroy();
        if (!ok) LOG_ERROR("OceanWaves: failed to create the pipeline for ", path);
        return ok;
    };
    if (!makePipeline("assets/shaders/ocean_spectrum.comp.spv", spectrumPipeline_) ||
        !makePipeline("assets/shaders/ocean_evolve.comp.spv", evolvePipeline_) ||
        !makePipeline("assets/shaders/ocean_fft.comp.spv", fftPipeline_) ||
        !makePipeline("assets/shaders/ocean_assemble.comp.spv", assemblePipeline_)) {
        return false;
    }

    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 3};
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("OceanWaves: failed to create the descriptor pool");
        return false;
    }
    VkDescriptorSetAllocateInfo setAlloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAlloc.descriptorPool = descPool_;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &setLayout_;
    if (vkAllocateDescriptorSets(device, &setAlloc, &set_) != VK_SUCCESS) {
        LOG_ERROR("OceanWaves: failed to allocate the descriptor set");
        return false;
    }
    VkDescriptorImageInfo images[3] = {
        {.sampler = VK_NULL_HANDLE, .imageView = state_.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = VK_NULL_HANDLE, .imageView = modes_.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = VK_NULL_HANDLE, .imageView = surface_.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet w[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
    return true;
}

void OceanWaves::advance(double deltaSeconds, float speedYardsPerSecond, glm::vec2 windDirection,
                         glm::vec2 nearby) {
    sea_.advance(deltaSeconds, speedYardsPerSecond, windDirection);
    pendingSeconds_ += std::max(deltaSeconds, 0.0);
    // The shaders measure from an origin near the player: the world's own
    // coordinates over a 4-yard patch run into the thousands, and a float
    // holds their fraction to a tenth of a texel. A whole number of patches,
    // so the waves do not move when it does.
    for (uint32_t c = 0; c < kCascades; ++c) {
        const double patch = glsl::oceanPatch(static_cast<int>(c));
        const double ox = std::floor(nearby.x / patch) * patch;
        const double oy = std::floor(nearby.y / patch) * patch;
        cascades_[c] = glm::vec4(static_cast<float>(ox), static_cast<float>(oy),
                                 static_cast<float>(1.0 / patch), static_cast<float>(patch));
    }
}

glm::vec4 OceanWaves::seaState() const {
    return {std::sqrt(sea_.heightVariance()), sea_.ride(), sea_.meanSquareSlope(), 0.0f};
}

void OceanWaves::record(VkCommandBuffer cmd) {
    ZoneScopedN("OceanWaves::record");
    if (!passesReady_ || cmd == VK_NULL_HANDLE) return;

    OceanPushGPU push{};
    push.toward = sea_.toward();
    push.speed = sea_.speed();
    push.slopeScale = sea_.slopeScale();
    push.deltaTime = static_cast<float>(pendingSeconds_);
    push.time = sea_.time();
    push.ride = sea_.ride();
    push.restart = restartPending_ ? 1 : 0;
    push.vertical = 0;
    pendingSeconds_ = 0.0;
    restartPending_ = false;

    // The last frame's water read the surface this overwrites, and its
    // passes wrote what this one reads.
    memoryBarrier(cmd,
                  VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                  VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const auto computeToCompute = [&] {
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, spectrumPipeline_);
    vkCmdDispatch(cmd, kSize / kGroup, kSize / kGroup, kCascades);
    computeToCompute();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, evolvePipeline_);
    vkCmdDispatch(cmd, kSize / kGroup, kSize / kGroup, kCascades);
    computeToCompute();

    // A workgroup a line of a layer: the rows, then the columns.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fftPipeline_);
    vkCmdDispatch(cmd, 1, kSize, 2 * kCascades);
    computeToCompute();
    push.vertical = 1;
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, 1, kSize, 2 * kCascades);
    computeToCompute();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, assemblePipeline_);
    vkCmdDispatch(cmd, kSize / kGroup, kSize / kGroup, kCascades);

    recordMips(cmd);
}

void OceanWaves::recordMips(VkCommandBuffer cmd) {
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    // Each mip the average of the one above: the mean displacement, slope
    // and square of slope over its texel.
    for (uint32_t level = 1; level < surfaceMips_; ++level) {
        const int32_t from = static_cast<int32_t>(kSize >> (level - 1));
        const int32_t to = static_cast<int32_t>(kSize >> level);
        VkImageBlit blit{};
        blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level - 1,
                               .baseArrayLayer = 0, .layerCount = 3 * kCascades};
        blit.srcOffsets[1] = {from, from, 1};
        blit.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level,
                               .baseArrayLayer = 0, .layerCount = 3 * kCascades};
        blit.dstOffsets[1] = {to, to, 1};
        vkCmdBlitImage(cmd, surface_.image, VK_IMAGE_LAYOUT_GENERAL, surface_.image, VK_IMAGE_LAYOUT_GENERAL,
                       1, &blit, VK_FILTER_LINEAR);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    // The water reads it from its vertices, its tessellated ones and its
    // pixels.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);
}

void OceanWaves::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    ctx_->waitIdle();
    {
        DeferredResources retired(*ctx_);
        retired.release(surfaceSampledView_, vkDestroyImageView);
    }
    destroyImage(state_);
    destroyImage(modes_);
    destroyImage(surface_);
    destroy(device, descPool_);
    set_ = VK_NULL_HANDLE;
    destroy(device, spectrumPipeline_);
    destroy(device, evolvePipeline_);
    destroy(device, fftPipeline_);
    destroy(device, assemblePipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, setLayout_);
    sampler_ = VK_NULL_HANDLE;  // the context's cache owns it
    passesReady_ = false;
    restartPending_ = true;
    ctx_ = nullptr;
}

} // namespace rendering
} // namespace wowee
