#include "rendering/renderer.hpp"
#include "rendering/frame_scene.hpp"
#include "collision/collision_world.hpp"
#include "rendering/offscreen_composite.hpp"
#include "rendering/frame_sink.hpp"
#include "rendering/sun_direction.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include "core/env_flag.hpp"
#include "rendering/sky_params_from_lighting.hpp"
#include "core/coordinates.hpp"
#include "rendering/camera.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/camera_water_depth.hpp"
#include "pipeline/custom_zone_discovery.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/sea_bed.hpp"
#include "rendering/water_reflection.hpp"
#include "rendering/skybox.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/cloud_coverage.hpp"
#include "rendering/sky_bodies.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/shelter_map.hpp"
#include "rendering/precipitation.hpp"
#include "rendering/lens_weather.hpp"
#include "rendering/lightning.hpp"
#include "rendering/lighting_manager.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/mount_dust.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/wmo_night_glow.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/grass_profile.hpp"
#include "pipeline/grass_population.hpp"
#include "pipeline/grass_terrain.hpp"
#include "rendering/grass_renderer.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/volumetric_fog.hpp"
#include "rendering/sky_background.hpp"
#include "rendering/sun_shafts.hpp"
#include "rendering/bloom.hpp"
#include "rendering/distant_terrain.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/screen_capture.hpp"
#include "rendering/loot_sparkles.hpp"
#include "rendering/corpse_bones.hpp"
#include "rendering/minimap.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/footprint_renderer.hpp"
#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "rendering/render_surface.hpp"
#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/amd_fsr3_runtime.hpp"
#include "rendering/character_emitters.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/render_graph.hpp"
#include "rendering/overlay_system.hpp"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cctype>
#include <cmath>
#include <chrono>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <future>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace wowee {
namespace rendering {



Renderer::Renderer()
    : collision_(std::make_unique<collision::CollisionWorld>()),
      scene_(std::make_unique<SceneCommands>(*this)) {}
Renderer::~Renderer() = default;

void Renderer::applySceneCommands() {
    scene_->apply();
}

bool Renderer::createPerFrameResources() {
    VkDevice device = vkCtx->getDevice();

    // --- Create per-frame shadow depth images (one per in-flight frame) ---
    // Each frame slot has its own depth image so that frame N's shadow read and
    // frame N+1's shadow write cannot race on the same image.
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_D32_SFLOAT;
    imgCI.extent = {.width = SHADOW_MAP_SIZE, .height = SHADOW_MAP_SIZE, .depth = 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo imgAllocCI{};
    imgAllocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (gpuCreateImage(vkCtx->getAllocator(), &imgCI, &imgAllocCI,
                &shadowDepthImage[i], &shadowDepthAlloc[i], nullptr) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image [", i, "]");
            return false;
        }
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // --- Create per-frame shadow depth image views ---
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_D32_SFLOAT;
    viewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        viewCI.image = shadowDepthImage[i];
        if (vkCreateImageView(device, &viewCI, nullptr, &shadowDepthView[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image view [", i, "]");
            return false;
        }
    }

    // --- Create shadow sampler (shared - read-only, no per-frame needed) ---
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampCI.compareEnable = VK_TRUE;
    sampCI.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowSampler = vkCtx->getOrCreateSampler(sampCI);
    if (shadowSampler == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create shadow sampler");
        return false;
    }

    // --- Create shadow render pass (depth-only) ---
    VkAttachmentDescription depthAtt{};
    depthAtt.format = VK_FORMAT_D32_SFLOAT;
    depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments = &depthAtt;
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;
    if (vkCreateRenderPass(device, &rpCI, nullptr, &shadowRenderPass) != VK_SUCCESS) {
        LOG_ERROR("Failed to create shadow render pass");
        return false;
    }

    // --- Create per-frame shadow framebuffers ---
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = shadowRenderPass;
    fbCI.attachmentCount = 1;
    fbCI.width = SHADOW_MAP_SIZE;
    fbCI.height = SHADOW_MAP_SIZE;
    fbCI.layers = 1;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        fbCI.pAttachments = &shadowDepthView[i];
        if (vkCreateFramebuffer(device, &fbCI, nullptr, &shadowFramebuffer[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow framebuffer [", i, "]");
            return false;
        }
    }

    // The fog's sampler and neutral volume come first: the layout below bakes
    // the one in, and every set written below binds the other until the fog
    // has volumes of its own.
    volumetricFog_ = std::make_unique<VolumetricFog>();
    if (!volumetricFog_->initialize(vkCtx)) {
        LOG_ERROR("Failed to create the volumetric fog's sampler and neutral volume");
        return false;
    }

    // The ray traced lighting's scene and pass. They outlive the per-frame
    // sets: the renderers register geometry with the scene as they load, and
    // bindings 3 and 4 below take the pass's sampler as immutable.
    if (!rtScene_) {
        rtScene_ = std::make_unique<RtScene>();
        if (!rtScene_->initialize(vkCtx)) {
            LOG_ERROR("Failed to create the ray tracing scene");
            return false;
        }
        rtLighting_ = std::make_unique<RtLighting>();
        if (!rtLighting_->initialize(vkCtx, rtScene_.get())) {
            LOG_ERROR("Failed to create the ray traced lighting");
            return false;
        }
    }

    // A small local depth map for geometry contact shadows. Its footprint
    // stays fixed when the player increases distant sun-shadow coverage.
    VkSamplerCreateInfo contactSamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    contactSamplerInfo.magFilter = contactSamplerInfo.minFilter = VK_FILTER_NEAREST;
    contactSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    contactSamplerInfo.addressModeU = contactSamplerInfo.addressModeV =
        contactSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    contactSamplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    contactShadowSampler_ = vkCtx->getOrCreateSampler(contactSamplerInfo);
    if (!contactShadowSampler_) return false;
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        contactShadowImages_[i] = createImage(device, vkCtx->getAllocator(),
            CONTACT_MAP_SIZE, CONTACT_MAP_SIZE, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!contactShadowImages_[i].image) return false;
        VkFramebufferCreateInfo contactFB{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        contactFB.renderPass = shadowRenderPass;
        contactFB.attachmentCount = 1;
        contactFB.pAttachments = &contactShadowImages_[i].imageView;
        contactFB.width = contactFB.height = CONTACT_MAP_SIZE;
        contactFB.layers = 1;
        if (vkCreateFramebuffer(device, &contactFB, nullptr, &contactShadowFramebuffer_[i]) != VK_SUCCESS)
            return false;
        lightningShadowImages_[i] = createImage(device, vkCtx->getAllocator(),
            LIGHTNING_MAP_SIZE, LIGHTNING_MAP_SIZE, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!lightningShadowImages_[i].image) {
            LOG_ERROR("Lightning shadow map allocation failed for frame ", i, "; renderer initialization failed");
            return false;
        }
        VkFramebufferCreateInfo lightningFB{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        lightningFB.renderPass = shadowRenderPass;
        lightningFB.attachmentCount = 1;
        lightningFB.pAttachments = &lightningShadowImages_[i].imageView;
        lightningFB.width = lightningFB.height = LIGHTNING_MAP_SIZE;
        lightningFB.layers = 1;
        if (vkCreateFramebuffer(device, &lightningFB, nullptr, &lightningShadowFramebuffer_[i]) != VK_SUCCESS) {
            LOG_ERROR("Lightning shadow framebuffer failed for frame ", i, "; renderer initialization failed");
            return false;
        }
    }
    // Bindings must be valid before the first world/preview draw as well.
    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        for (auto* images : {&contactShadowImages_, &lightningShadowImages_}) for (auto& image : *images) {
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier2 initial{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            initial.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            initial.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            initial.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            initial.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            initial.srcQueueFamilyIndex = initial.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            initial.image = image.image;
            initial.subresourceRange = range;
            VkDependencyInfo initialDep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            initialDep.imageMemoryBarrierCount = 1;
            initialDep.pImageMemoryBarriers = &initial;
            cmdPipelineBarrier2(cmd, initialDep);
            const VkClearDepthStencilValue clear{1.0f, 0};
            vkCmdClearDepthStencilImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       &clear, 1, &range);
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image.image;
            b.subresourceRange = range;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            cmdPipelineBarrier2(cmd, dep);
        }
    });
    for (auto& layout : contactShadowLayout_) layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (auto& layout : lightningShadowLayout_) layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // --- Create descriptor set layout for set 0 (per-frame UBO + shadow sampler + fog volume
    //     + ray traced lighting) ---
    VkDescriptorSetLayoutBinding bindings[11]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    // Compute as well: the fog volume is lit from this same block.
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;
    // And the water's tessellation, which divides and moves it by the camera.
    if (vkCtx->isTessellationSupported()) {
        bindings[0].stageFlags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Immutable, because this one compares. Portability implementations may
    // report VkPhysicalDevicePortabilitySubsetFeaturesKHR::mutableComparisonSamplers
    // as false -- MoltenVK does -- and then a sampler with compareEnable set is
    // only legal here, baked into the layout, rather than written into the
    // descriptor per frame. shadowSampler is created above this point.
    bindings[1].pImmutableSamplers = &shadowSampler;
    // The fog volume, read per fragment by surfaces and per vertex by
    // particles and ribbons. Immutable like binding 1, so a set allocated
    // anywhere else - the character preview's - only has to name a view.
    const VkSampler fogSampler = volumetricFog_->getSampler();
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].pImmutableSamplers = &fogSampler;
    // Last frame's ray traced lighting (RtLighting), read by the terrain,
    // building, doodad and character surfaces. Immutable samplers again, so
    // the preview's sets only name the neutral view.
    const VkSampler rtSampler = rtLighting_->sampler();
    for (uint32_t b = 3; b <= 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[b].pImmutableSamplers = &rtSampler;
    }

    // Real sky color for the far atmospheric background. Linear filtering
    // smooths the small sky image; immutable so previews need only a view.
    VkSamplerCreateInfo skySamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    skySamplerInfo.magFilter = skySamplerInfo.minFilter = VK_FILTER_LINEAR;
    skySamplerInfo.addressModeU = skySamplerInfo.addressModeV = skySamplerInfo.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const VkSampler skySampler = vkCtx->getOrCreateSampler(skySamplerInfo);
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].pImmutableSamplers = &skySampler;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    for (uint32_t b = 6; b <= 7; ++b) {
        bindings[b] = bindings[3];
        bindings[b].binding = b;
    }
    bindings[8] = bindings[3];
    bindings[8].binding = 8;
    bindings[8].pImmutableSamplers = &contactShadowSampler_;
    // The frame's point lights (local_lights.glsli), read by surfaces and
    // by the fog volume.
    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[10] = bindings[8];
    bindings[10].binding = 10;
    bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    layoutInfo.bindingCount = 11;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &perFrameSetLayout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create per-frame descriptor set layout");
        return false;
    }

    // --- Create descriptor pool for UBO + image sampler (normal frames + reflection) ---
    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES * 3; // world, capture and reflection sets
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES * 3 * 9;  // shadow, fog, lighting, sky, reflection and geometry
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = MAX_FRAMES * 3;  // the point lights

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES * 3; // world, capture and reflection sets
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &sceneDescriptorPool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create scene descriptor pool");
        return false;
    }

    // --- Create per-frame UBOs and descriptor sets ---
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        // Create mapped UBO
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (gpuCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                &perFrameUBOs[i], &perFrameUBOAllocs[i], &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create per-frame UBO ", i);
            return false;
        }
        perFrameUBOMapped[i] = mapInfo.pMappedData;

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameSetLayout;

        if (vkAllocateDescriptorSets(device, &setAlloc, &perFrameDescSets[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate per-frame descriptor set ", i);
            return false;
        }

        // Write binding 0 (UBO) and binding 1 (shadow sampler)
        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = perFrameUBOs[i];
        descBuf.offset = 0;
        descBuf.range = sizeof(GPUPerFrameData);

        VkDescriptorImageInfo shadowImgInfo{};
        // sampler is ignored: binding 1 declares it immutable in the layout.
        shadowImgInfo.imageView = shadowDepthView[i];
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Neutral until the fog is switched on; writeFogVolumeBindings swaps
        // in this slot's own volume then.
        VkDescriptorImageInfo fogImgInfo{};
        fogImgInfo.imageView = volumetricFog_->getVolumeView(i);
        fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Neutral until the ray traced lighting is on; writeRtLightingBindings
        // swaps in the other slot's results then.
        VkDescriptorImageInfo rtImgInfo[4]{};
        rtImgInfo[0].imageView = rtLighting_->lightViewForSlot(i);
        rtImgInfo[1].imageView = rtLighting_->giViewForSlot(i);
        rtImgInfo[2].imageView = rtLighting_->reflectionViewForSlot(i);
        rtImgInfo[3].imageView = rtLighting_->geometryViewForSlot(i);
        for (auto& info : rtImgInfo) info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[10]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = perFrameDescSets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = perFrameDescSets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImgInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = perFrameDescSets[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &fogImgInfo;
        for (uint32_t b = 0; b < 2; ++b) {
            writes[3 + b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3 + b].dstSet = perFrameDescSets[i];
            writes[3 + b].dstBinding = 3 + b;
            writes[3 + b].descriptorCount = 1;
            writes[3 + b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3 + b].pImageInfo = &rtImgInfo[b];
        }

        writes[5] = writes[4];
        writes[5].dstBinding = 5;
        for (uint32_t b = 6; b <= 7; ++b) {
            writes[b] = writes[4];
            writes[b].dstBinding = b;
            writes[b].pImageInfo = &rtImgInfo[b - 4];
        }
        VkDescriptorImageInfo contactInfo{};
        contactInfo.imageView = contactShadowImages_[i].imageView;
        contactInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[8] = writes[1];
        writes[8].dstBinding = 8;
        writes[8].pImageInfo = &contactInfo;
        VkDescriptorImageInfo lightningInfo{};
        lightningInfo.imageView = lightningShadowImages_[i].imageView;
        lightningInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[9] = writes[1];
        writes[9].dstBinding = 10;
        writes[9].pImageInfo = &lightningInfo;
        vkUpdateDescriptorSets(device, 10, writes, 0, nullptr);
        // Sky capture gets a separate set whose binding 5 stays neutral:
        // the M2 shader also declares the surface sampler, and must never
        // bind the image being written by this pass as a sampled image.
        setAlloc.pSetLayouts = &perFrameSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &skyPerFrameDescSets[i]) != VK_SUCCESS)
            return false;
        for (auto& write : writes) write.dstSet = skyPerFrameDescSets[i];
        vkUpdateDescriptorSets(device, 10, writes, 0, nullptr);
    }

    skyBackground_ = std::make_unique<SkyBackground>();

    // --- Create reflection per-frame UBO and descriptor set ---
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        for (uint32_t i = 0; i < MAX_FRAMES; i++) {
            VmaAllocationInfo mapInfo{};
            if (gpuCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                    &reflPerFrameUBO[i], &reflPerFrameUBOAlloc[i], &mapInfo) != VK_SUCCESS) {
                LOG_ERROR("Failed to create reflection per-frame UBO for frame slot ", i);
                return false;
            }
            reflPerFrameUBOMapped[i] = mapInfo.pMappedData;
        }

        VkDescriptorSetLayout layouts[MAX_FRAMES];
        for (auto& layout : layouts) layout = perFrameSetLayout;

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = MAX_FRAMES;
        setAlloc.pSetLayouts = layouts;

        if (vkAllocateDescriptorSets(device, &setAlloc, reflPerFrameDescSet) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate reflection per-frame descriptor sets");
            return false;
        }

        // Bind each reflection descriptor to its own frame's UBO and shadow view
        for (uint32_t i = 0; i < MAX_FRAMES; i++) {
            VkDescriptorBufferInfo descBuf{};
            descBuf.buffer = reflPerFrameUBO[i];
            descBuf.offset = 0;
            descBuf.range = sizeof(GPUPerFrameData);

            VkDescriptorImageInfo shadowImgInfo{};
            // sampler is ignored: binding 1 declares it immutable in the layout.
            shadowImgInfo.imageView = shadowDepthView[i];
            shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // Always neutral: the volume is built for the camera, and the
            // mirrored one would read it at the wrong place. The reflection's
            // block switches the fog off.
            VkDescriptorImageInfo fogImgInfo{};
            fogImgInfo.imageView = volumetricFog_->getNeutralView();
            fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            // Neutral as well: the result is the camera's, and the block's
            // switch is off in the reflection's copy.
            VkDescriptorImageInfo rtImgInfo{};
            rtImgInfo.imageView = rtLighting_->neutralView();
            rtImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[10]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = reflPerFrameDescSet[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &descBuf;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = reflPerFrameDescSet[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &shadowImgInfo;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = reflPerFrameDescSet[i];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &fogImgInfo;
            for (uint32_t b = 3; b <= 4; ++b) {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = reflPerFrameDescSet[i];
                writes[b].dstBinding = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b].pImageInfo = &rtImgInfo;
            }

            writes[5] = writes[4];
            writes[5].dstBinding = 5;
            for (uint32_t b = 6; b <= 7; ++b) {
                writes[b] = writes[4];
                writes[b].dstBinding = b;
            }
            VkDescriptorImageInfo contactInfo{};
            contactInfo.imageView = contactShadowImages_[i].imageView;
            contactInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[8] = writes[1];
            writes[8].dstBinding = 8;
            writes[8].pImageInfo = &contactInfo;
            VkDescriptorImageInfo lightningInfo{};
            lightningInfo.imageView = lightningShadowImages_[i].imageView;
            lightningInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[9] = writes[1];
            writes[9].dstBinding = 10;
            writes[9].pImageInfo = &lightningInfo;
            vkUpdateDescriptorSets(device, 10, writes, 0, nullptr);
        }
    }

    // The point lights: a buffer per frame slot, grown as a frame needs, and
    // an empty one for sets allocated elsewhere.
    if (!createLightBuffer(neutralLightBuffer_, 1)) return false;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (!createLightBuffer(localLightBuffers_[i], kInitialLocalLightCapacity)) return false;
        writeLightBinding(i);
    }

    // The compute side. Not fatal: without it the fog simply stays off, and
    // applyPendingQuality says so when it is asked for.
    if (!volumetricFog_->createPipelines(perFrameSetLayout, shadowDepthView)) {
        LOG_WARNING("Volumetric fog pipelines failed to build - volumetric fog unavailable");
    }

    LOG_INFO("Per-frame Vulkan resources created (shadow map ", SHADOW_MAP_SIZE, "x", SHADOW_MAP_SIZE, ")");
    return true;
}

bool Renderer::createLightBuffer(LightBuffer& out, size_t lights) {
    // Two vec4 per light (local_lights.glsli).
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = std::max<size_t>(lights, 1) * 2 * sizeof(glm::vec4);
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapInfo{};
    if (gpuCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo, &out.buffer, &out.alloc,
                        &mapInfo) != VK_SUCCESS) {
        LOG_ERROR("Failed to create the point light buffer for ", lights, " lights");
        out = {};
        return false;
    }
    out.mapped = mapInfo.pMappedData;
    out.capacity = std::max<size_t>(lights, 1);
    std::memset(out.mapped, 0, static_cast<size_t>(bufInfo.size));
    return true;
}

void Renderer::writeLightBinding(uint32_t slot) {
    VkDescriptorBufferInfo info{};
    info.buffer = localLightBuffers_[slot].buffer;
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[3]{};
    uint32_t count = 0;
    // The world's set, the sky capture's and the reflection's: the same
    // lights in all three.
    for (VkDescriptorSet set : {perFrameDescSets[slot], skyPerFrameDescSets[slot], reflPerFrameDescSet[slot]}) {
        if (set == VK_NULL_HANDLE) continue;
        VkWriteDescriptorSet& w = writes[count++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 9;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &info;
    }
    vkUpdateDescriptorSets(vkCtx->getDevice(), count, writes, 0, nullptr);
}

void Renderer::uploadLocalLights(uint32_t slot) {
    LightBuffer& buf = localLightBuffers_[slot];
    const size_t count = std::min<size_t>(frameLocalLights_.size(), kMaxAddressableLocalLights);
    if (frameLocalLights_.size() > kMaxAddressableLocalLights) {
        static bool said = false;
        if (!said) {
            said = true;
            LOG_WARNING("Local lights: ", frameLocalLights_.size(), " in one frame; only the first ",
                        kMaxAddressableLocalLights, " can be indexed and the rest light nothing");
        }
    }
    if (count > buf.capacity && !buf.growFailed) {
        // This slot's frame has finished on the GPU (beginFrame waited for
        // it) and nothing has been recorded with its sets yet this frame, so
        // the buffer can be replaced and the sets pointed at the new one.
        // The new one is made first: the sets keep pointing at the old buffer
        // until there is something to point them at instead.
        const size_t grown = std::max(count, buf.capacity * 2);
        LightBuffer bigger;
        if (createLightBuffer(bigger, grown)) {
            destroy(vkCtx->getAllocator(), buf.buffer, buf.alloc);
            buf = bigger;
            writeLightBinding(slot);
        } else {
            buf.growFailed = true;
            LOG_WARNING("Local lights: frame slot ", slot, " cannot grow past ", buf.capacity,
                        " lights; the first ", buf.capacity, " of each frame are drawn and the rest light nothing");
        }
    }
    // Draws pick their lights from frameLocalLights_ by index and the shaders
    // read those indices from the bound buffer unchecked, so the list is cut
    // to what the buffer holds before any draw sees it.
    const size_t uploaded = std::min(count, buf.capacity);
    if (frameLocalLights_.size() > uploaded) frameLocalLights_.resize(uploaded);
    auto* dst = static_cast<glm::vec4*>(buf.mapped);
    for (size_t i = 0; i < uploaded; ++i) {
        dst[2 * i] = glm::vec4(frameLocalLights_[i].position, 1.0f);
        dst[2 * i + 1] = glm::vec4(frameLocalLights_[i].color, 1.0f);
    }
    currentFrameData.localLightMeta = glm::ivec4(static_cast<int32_t>(uploaded), 0, 0, 0);
}

void Renderer::destroyPerFrameResources() {
    if (!vkCtx) return;
    vkCtx->waitIdle();
    VkDevice device = vkCtx->getDevice();

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroy(vkCtx->getAllocator(), perFrameUBOs[i], perFrameUBOAllocs[i]);
        destroy(vkCtx->getAllocator(), localLightBuffers_[i].buffer, localLightBuffers_[i].alloc);
        localLightBuffers_[i] = {};
    }
    destroy(vkCtx->getAllocator(), neutralLightBuffer_.buffer, neutralLightBuffer_.alloc);
    neutralLightBuffer_ = {};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (!reflPerFrameUBO[i]) continue;
        gpuDestroyBuffer(vkCtx->getAllocator(), reflPerFrameUBO[i], reflPerFrameUBOAlloc[i]);
        reflPerFrameUBO[i] = VK_NULL_HANDLE;
        reflPerFrameUBOMapped[i] = nullptr;
    }
    // Before the layout: its compute pipeline layout was built from it.
    if (volumetricFog_) {
        volumetricFog_->shutdown();
        volumetricFog_.reset();
    }
    if (skyBackground_) { skyBackground_->shutdown(); skyBackground_.reset(); }
    destroy(device, sceneDescriptorPool);
    destroy(device, perFrameSetLayout);
    // After every renderer has let go of its geometry, which is why this is
    // here rather than beside the fog: shutdown() calls this last.
    if (rtLighting_) {
        rtLighting_->shutdown();
        rtLighting_.reset();
    }
    if (rtScene_) {
        rtScene_->shutdown();
        rtScene_.reset();
    }

    // Destroy per-frame shadow resources
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (shadowFramebuffer[i]) { vkDestroyFramebuffer(device, shadowFramebuffer[i], nullptr); shadowFramebuffer[i] = VK_NULL_HANDLE; }
        if (shadowDepthView[i]) { vkDestroyImageView(device, shadowDepthView[i], nullptr); shadowDepthView[i] = VK_NULL_HANDLE; }
        if (shadowDepthImage[i]) { gpuDestroyImage(vkCtx->getAllocator(), shadowDepthImage[i], shadowDepthAlloc[i]); shadowDepthImage[i] = VK_NULL_HANDLE; shadowDepthAlloc[i] = VK_NULL_HANDLE; }
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        if (contactShadowFramebuffer_[i]) {
            vkDestroyFramebuffer(device, contactShadowFramebuffer_[i], nullptr);
            contactShadowFramebuffer_[i] = VK_NULL_HANDLE;
        }
        destroyImage(device, vkCtx->getAllocator(), contactShadowImages_[i]);
        if (lightningShadowFramebuffer_[i]) {
            vkDestroyFramebuffer(device, lightningShadowFramebuffer_[i], nullptr);
            lightningShadowFramebuffer_[i] = VK_NULL_HANDLE;
        }
        destroyImage(device, vkCtx->getAllocator(), lightningShadowImages_[i]);
    }
    contactShadowSampler_ = VK_NULL_HANDLE;
    if (shadowRenderPass) { vkDestroyRenderPass(device, shadowRenderPass, nullptr); shadowRenderPass = VK_NULL_HANDLE; }
    shadowSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
}

void Renderer::updatePerFrameUBO() {
    if (!frameCamera()) return;

    currentFrameData.view = frameCamera()->getViewMatrix();
    currentFrameData.projection = frameCamera()->getProjectionMatrix();
    currentFrameData.viewPos = glm::vec4(frameCamera()->getPosition(), 1.0f);
    currentFrameData.fogParams.z = frameInputs_.time;
    const WindGpu wind = frameInputs_.wind;
    currentFrameData.windDirLean = wind.dirLean;
    currentFrameData.windPhases = wind.phases;
    currentFrameData.windTravel = wind.travel;

    currentFrameData.aerialParams=glm::vec4(0);
    currentFrameData.terrainLodParams=glm::vec4(kDistantTerrainStart,kDistantTerrainEnd,frameInputs_.distantTerrainReady ? 1.f : 0.f,0.f);
    currentFrameData.fogSkyTop.w = 0.0f;
    currentFrameData.fogSkyMiddle.w = 0.0f; // actual sky image becomes valid in renderWorld
    currentFrameData.fogSkyBand1.w = frameInputs_.viewDistance; // common horizontal geometry range

    // Lighting from LightingManager, with the weather's haze over it.
    if (frameInputs_.hasLighting) {
        const auto& lp = frameInputs_.weatherLight;
        currentFrameData.lightDir = glm::vec4(
            lp.directionalDir, wmoNightGlow(frameInputs_.timeOfDay / 24.0f));
        // Looking into the sun dims the scene's light by up to a third, as the
        // client dims its direct and ambient colours (0x781783) - by the glare
        // the eye can see, so not behind a hill or after dark.
        float lookingAtSun = 0.0f;
        if (frameCamera()) {
            const glm::vec3 discDir =
                sky_celestial::sunDisc(frameInputs_.timeOfDay / 24.0f).direction;
            lookingAtSun = sky_celestial::lookingAtSunDimming(
                glm::dot(glm::normalize(frameCamera()->getForward()), discDir), frameInputs_.sunGlare);
        }
        currentFrameData.lightColor = glm::vec4(lp.diffuseColor * (1.0f - lookingAtSun), 1.0f);
        currentFrameData.ambientColor = glm::vec4(lp.ambientColor * (1.0f - lookingAtSun), 1.0f);
        currentFrameData.fogColor = glm::vec4(lp.fogColor, 1.0f);
        currentFrameData.fogParams.x = lp.fogStart;
        currentFrameData.fogParams.y = lp.fogEnd;
        currentFrameData.fogSkyTop = glm::vec4(lp.skyTopColor, 0.0f);
        currentFrameData.fogSkyMiddle = glm::vec4(lp.skyMiddleColor, 0.0f);
        currentFrameData.fogSkyBand1 = glm::vec4(lp.skyBand1Color, frameInputs_.viewDistance);
        currentFrameData.fogSkyBand2 = glm::vec4(lp.skyBand2Color, 0.0f);
        currentFrameData.skySmogColor = glm::vec4(lp.skySmogColor, 0.0f);
        const auto disc=sky_celestial::sunDisc(frameInputs_.timeOfDay/24.f);
        const float day=glm::smoothstep(-0.04f,0.15f,disc.direction.z);
        const glm::vec3 illumination=glm::min(lp.ambientColor+lp.diffuseColor,glm::vec3(1));
        const float grey=glm::dot(lp.skySmogColor,glm::vec3(.2126f,.7152f,.0722f));
        currentFrameData.aerialSun=glm::vec4(disc.direction,day);
        currentFrameData.aerialLight=glm::vec4(glm::mix(glm::vec3(grey),illumination,.65f),0);
        const float thickness=frameInputs_.fogStrength;
        if(frameInputs_.worldInView && thickness>0.001f) {
            // Enable the shared sky/surface colour correction only. Opacity
            // uses fogParams' zone start/end, scaled by LightingManager.
            currentFrameData.aerialParams=glm::vec4(0,0,0,1);
        }

        // Shift fog to blue when camera is significantly underwater (terrain water only).
        {
            constexpr float MIN_SUBMERSION = 2.0f;
            const auto eyeDepth = frameInputs_.cameraWaterDepth;
            if (eyeDepth && *eyeDepth > MIN_SUBMERSION) {
                float depth = *eyeDepth - MIN_SUBMERSION;
                float blend = glm::clamp(1.0f - std::exp(-depth * 0.08f), 0.0f, 0.7f);
                glm::vec3 underwaterFog(0.03f, 0.09f, 0.18f);
                glm::vec3 blendedFog = glm::mix(lp.fogColor, underwaterFog, blend);
                currentFrameData.aerialParams.w=0;
                currentFrameData.fogColor = glm::vec4(blendedFog, 1.0f);
                currentFrameData.fogSkyTop.w = 0.0f;
                currentFrameData.fogParams.x = glm::mix(lp.fogStart, 20.0f, blend);
                currentFrameData.fogParams.y = glm::mix(lp.fogEnd, 200.0f, blend);
            }
        }
    }

    currentFrameData.lightSpaceMatrix = lightSpaceMatrix;
    currentFrameData.contactSpaceMatrix = glm::mat4(0.0f);
    currentFrameData.contactParams = glm::vec4(0.0f);
    if (frameInputs_.shadowMatrix != glm::mat4(0.0f) && contactShadowImages_[0].image) {
        // The units' map, as the client keeps it: 40 yards a side around the
        // player (WoW.exe 12340 sets its half extent to 20 yards), along the
        // same rays as the sun map.
        glm::vec3 rays = glm::vec3(currentFrameData.lightDir);
        const float lengthSq = glm::dot(rays, rays);
        rays = lengthSq > 1e-6f ? rays * glm::inversesqrt(lengthSq)
                               : glm::normalize(glm::vec3(-0.3f, -0.7f, -0.6f));
        if (rays.z > 0.0f) rays = -rays;
        if (rays.z > -0.15f) rays = glm::normalize(glm::vec3(rays.x, rays.y, -0.15f));
        const glm::vec3 up = std::abs(rays.z) > 0.99f
            ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        glm::mat4 projection = glm::ortho(-CONTACT_MAP_HALF_EXTENT, CONTACT_MAP_HALF_EXTENT,
                                          -CONTACT_MAP_HALF_EXTENT, CONTACT_MAP_HALF_EXTENT,
                                          0.1f, 128.0f);
        projection[1][1] *= -1.0f;
        currentFrameData.contactSpaceMatrix = projection * glm::lookAt(
            frameInputs_.characterPosition - rays * 64.0f, frameInputs_.characterPosition, up);
        static const bool contactEnabled = std::getenv("WOWEE_DISABLE_CONTACT_SHADOWS") == nullptr;
        currentFrameData.contactParams = glm::vec4(contactEnabled ? 1.0f : 0.0f,
                                                  1.0f / CONTACT_MAP_SIZE, 127.9f, 0.0f);
    }
    // z carries one texel of the shadow map. The shaders used to hold that as
    // a constant for 4096, and the map is 512 to 4096 by the quality level.
    //
    // y was a shadow strength that grew with the shadow distance - a third of
    // a shadow at 100 yards, all of it from 375. A shadow is whole at every
    // distance; how dark it is is the client's fixed 30% (shadow_shading.glsli).
    currentFrameData.shadowParams = glm::vec4(frameInputs_.shadows ? 1.0f : 0.0f, 0.0f,
                                              1.0f / static_cast<float>(SHADOW_MAP_SIZE),
                                              2.0f * frameInputs_.shadowDistance / static_cast<float>(SHADOW_MAP_SIZE));

    currentFrameData.lightningColor = frameInputs_.scene->lightning.lightColor;
    currentFrameData.lightningColor.w = 1.0f / LIGHTNING_MAP_SIZE;
    currentFrameData.lightningPosition = frameInputs_.scene->lightning.lightPosition;
    currentFrameData.lightningMatrix = frameInputs_.scene->lightning.lightMatrix;

    // Whether this frame builds the fog volume. Decided here, beside the
    // switch in the block the shaders read, and the frame graph dispatches by
    // the same flag - so no shader reads a volume its frame did not build.
    volumetricThisFrame_ = volumetricFog_ && volumetricFog_->isOn() &&
                           frameInputs_.fogDensity > 0.0f &&
                           !(passAblation_ && passAblation_->skip(AblationPass::VolumetricFog));
    currentFrameData.volumetricParams = volumetricThisFrame_ ? volumetricFog_->frameParams()
                                                             : glm::vec4(0.0f);

    // The frame's point lights: the M2Lights of the models and units animated
    // this frame, as the client lights a scene (local_lights.hpp). Each draw
    // then picks its nearest few from this list by index; the list itself has
    // no cap, and its buffer grows with it.
    std::optional<CpuStage> lightsStage(std::in_place, vkCtx->cpuStages(), "beginFrame/per-frame UBO",
                                        "local lights");
    frameLocalLights_ = frameInputs_.localLights;
    uploadLocalLights(vkCtx->getCurrentFrame());
    lightsStage.reset();
    const uint32_t localLightCount = static_cast<uint32_t>(currentFrameData.localLightMeta.x);

    if (rtLighting_) {
        const RtLighting::ConsumerData rt = rtLighting_->consumerData();
        currentFrameData.rtViewProj = rt.viewProj;
        currentFrameData.rtCameraPos = rt.cameraPos;
        currentFrameData.rtParams = rt.params;
    }

    // What the local lights are doing, a line every few seconds: how many
    // models in and around the view carry a lit light.
    {
        static double lastLightLog = 0.0;
        if (localLightCount > 0 && (frameInputs_.time - lastLightLog) > 3.0) {
            lastLightLog = frameInputs_.time;
            const glm::vec3& c0 = frameLocalLights_[0].color;
            LOG_INFO("localLights: count=", localLightCount,
                     " first rgb=(", c0.r, ",", c0.g, ",", c0.b, ")");
        }
    }

    currentFrameData.playerPos = frameInputs_.playerPos;
    currentFrameData.playerWake = frameInputs_.playerWake;

    // Copy to current frame's mapped UBO
    uint32_t frame = vkCtx->getCurrentFrame();
    std::memcpy(perFrameUBOMapped[frame], &currentFrameData, sizeof(GPUPerFrameData));
}

void Renderer::setShadowQualityLevel(int level) {
    // The top two levels are the same size on purpose, and it is worth
    // saying why before someone raises the last one to 8192 as an easy win
    // for modern hardware. Each level doubles the side, so it quadruples
    // the image: at 4096 a depth map is 64 MB and there are two of them,
    // one per frame in flight, which is 128 MB. 8192 would be 512 MB of
    // VRAM for shadows alone - not a step a machine that can run this is
    // certain to have spare, and the allocation failing at start-up is not
    // a path this renderer handles gently.
    constexpr uint32_t kShadowSideForLevel[] = {512, 1024, 2048, 4096, 4096};
    SHADOW_MAP_SIZE = kShadowSideForLevel[std::clamp(level, 0, 4)];
}

bool Renderer::initialize(RenderSurface* surface) {
    surface_ = surface;
    vkCtx = surface->getVkContext();
    requestedMsaaSamples_ = vkCtx->getMsaaSamples();
    deferredWorldInitEnabled_ = core::envFlagEnabled("WOWEE_DEFER_WORLD_SYSTEMS", true);
    LOG_INFO("Initializing renderer (Vulkan)");

    // Create camera (in front of Stormwind gate, looking north)
    camera = std::make_unique<Camera>();
    camera->setPosition(glm::vec3(-8900.0f, -170.0f, 150.0f));
    camera->setRotation(0.0f, -5.0f);
    camera->setAspectRatio(surface_->getAspectRatio());
    camera->setFov(60.0f);

    // Create per-frame UBO and descriptor sets
    if (!createPerFrameResources()) {
        LOG_ERROR("Failed to create per-frame Vulkan resources");
        return false;
    }

    // Initialize Vulkan sub-renderers (Phase 3)

    // Sky system (owns skybox, starfield, celestial, clouds, lens flare)
    skySystem = std::make_unique<SkySystem>();
    if (!skySystem->initialize(vkCtx, perFrameSetLayout)) {
        LOG_ERROR("Failed to initialize sky system");
        return false;
    }
    // Expose sub-components via renderer accessors
    skybox = nullptr;  // Owned by skySystem; access via skySystem->getSkybox()
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    lensWeather_ = std::make_unique<LensWeather>();
    if (!lensWeather_->initialize(vkCtx)) {
        LOG_WARNING("Lens weather unavailable - no rain, snow or sand drawn on the camera");
        lensWeather_->shutdown();
        lensWeather_.reset();
    }

    lightning = std::make_unique<Lightning>();
    if (!lightning->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Lightning effect initialization failed (non-fatal)");

    swimEffects = std::make_unique<SwimEffects>();
    syncSwimEffectsTargetPass();
    if (!swimEffects->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Swim effect initialization failed (non-fatal)");

    mountDust = std::make_unique<MountDust>();
    if (!mountDust->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Mount dust effect initialization failed (non-fatal)");

    chargeEffect = std::make_unique<ChargeEffect>();
    if (!chargeEffect->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Charge effect initialization failed (non-fatal)");

    levelUpEffect = std::make_unique<LevelUpEffect>();
    lootSparkles_ = std::make_unique<LootSparkles>();
    corpseBones_ = std::make_unique<CorpseBones>();

    // Non-fatal like the effects above: a device that cannot build the compute
    // pipeline still gets everything else, and isReady() gates both call sites.
    grassRenderer_ = std::make_unique<GrassRenderer>();
    if (!grassRenderer_->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Grass renderer initialization failed (non-fatal)");
    // The distance setting may have been applied before this existed.
    grassRenderer_->setCullDistance(grassDistance_);

    questMarkerRenderer = std::make_unique<QuestMarkerRenderer>();
    footprintRenderer = std::make_unique<FootprintRenderer>();

    LOG_INFO("Vulkan sub-renderers initialized (Phase 3)");

    // LightingManager doesn't use GL - initialize for data-only use
    lightingManager = std::make_unique<LightingManager>();

    // Create secondary command buffer resources for multithreaded rendering.
    //
    // WOWEE_SINGLE_THREAD_RECORD takes the fallback path instead. That path
    // records the world inline on one thread, which costs CPU time but marks
    // every pass separately - grass included, where the parallel path records
    // grass into the terrain secondary and so reports the two as one number.
    // Running a frame profile both ways says how much of "terrain" is grass,
    // and whether serialising the recording moves the frame time at all.
    if (std::getenv("WOWEE_PASS_ABLATION") != nullptr) {
        passAblation_ = std::make_unique<PassAblation>();
        LOG_WARNING("Pass ablation enabled - ", PassAblation::phaseCount(),
                    " phases over about ",
                    static_cast<int>(passAblation_->expectedRunMs() / 1000.0),
                    "s of being in the world, the first 10s of it settling. Stand "
                    "still outdoors, do not move the camera, and do not quit before "
                    "the table is logged.");
    }

    static const bool forceSingleThread = std::getenv("WOWEE_SINGLE_THREAD_RECORD") != nullptr;
    if (forceSingleThread) {
        LOG_INFO("WOWEE_SINGLE_THREAD_RECORD set - inline recording, one pass per GPU mark");
    } else if (!createSecondaryCommandResources()) {
        LOG_WARNING("Failed to create secondary command buffers - falling back to single-threaded rendering");
    }

    // Create PostProcessPipeline (§4.3 - owns FSR/FXAA/FSR2/FSR3/brightness)
    postProcessPipeline_ = std::make_unique<PostProcessPipeline>();
    postProcessPipeline_->initialize(vkCtx);
    // The ray traced lighting normally records where the water leaves the
    // scene pass. A multisampled scene in an off-screen target never leaves
    // it early, so the pass records here instead, as the upscaler takes over.
    postProcessPipeline_->setSceneClosedHook([this](VkCommandBuffer) {
        if (rtRecordedThisFrame_ || !postProcessPipeline_) return;
        recordRtLighting(postProcessPipeline_->getSceneDepthImage(),
                         postProcessPipeline_->getSceneRenderExtent(),
                         postProcessPipeline_->sceneDepthIsMsaa(), "endFrame/post-process");
    });

    distantTerrain_ = std::make_unique<DistantTerrain>();
    if(!distantTerrain_->initialize(*vkCtx,perFrameSetLayout)) distantTerrain_.reset();

    bloom_ = std::make_unique<Bloom>();
    if (!bloom_->initialize(vkCtx)) {
        LOG_WARNING("Bloom initialization failed - scene glow unavailable");
        bloom_.reset();
    }

    // Not fatal: without them the picture is only missing its rays.
    sunShafts_ = std::make_unique<SunShafts>();
    if (!sunShafts_->initialize(vkCtx)) {
        LOG_WARNING("Sun shafts failed to initialise - sun shafts unavailable");
        sunShafts_->shutdown();
        sunShafts_.reset();
    }

    // Create render graph and register virtual resources
    renderGraph_ = std::make_unique<RenderGraph>();

    // Create overlay system (selection circle + fullscreen overlay)
    overlaySystem_ = std::make_unique<OverlaySystem>(vkCtx);
    renderGraph_->registerResource("shadow_depth");
    renderGraph_->registerResource("volumetric_fog");
    renderGraph_->registerResource("ocean_surface");
    renderGraph_->registerResource("sea_bed");
    renderGraph_->registerResource("shelter");
    renderGraph_->registerResource("reflection_texture");
    renderGraph_->registerResource("scene_color");
    renderGraph_->registerResource("scene_depth");
    renderGraph_->registerResource("final_image");

    if (!interfaceRecorder_.initialize(*vkCtx, vkCtx->getOverlayRenderPass())) {
        LOG_ERROR("Renderer interface pipeline initialization failed");
        return false;
    }
    LOG_INFO("Renderer initialized");
    return true;
}

void Renderer::shutdown() {
    frameInputs_.interface.reset();
    frameInputs_.auxiliaryInterface = {};
    frameInputs_.scene.reset();
    flushFrameCapture();
    endFrameCapture();
    interfaceRecorder_.shutdown();

    destroySecondaryCommandResources();

    LOG_DEBUG("Renderer::shutdown - terrainManager stopWorkers...");
    if (terrainManager) {
        terrainManager->stopWorkers();
        LOG_DEBUG("Renderer::shutdown - terrainManager reset...");
        standaloneTerrain_.reset();
        terrainManager = nullptr;
    }

    LOG_DEBUG("Renderer::shutdown - terrainRenderer...");
    if (terrainRenderer) {
        terrainRenderer->shutdown();
        terrainRenderer.reset();
    }

    LOG_DEBUG("Renderer::shutdown - waterRenderer...");
    waterSubscription_.reset();
    destroyOffscreenWater1xFramebuffer();
    if (waterRenderer) {
        waterRenderer->shutdown();
        waterRenderer.reset();
    }
    if (seaBed_) {
        seaBed_->shutdown();
        seaBed_.reset();
    }

    LOG_DEBUG("Renderer::shutdown - minimap...");
    if (minimap) {
        minimap->shutdown();
        minimap.reset();
    }

    if (distantTerrain_) distantTerrain_.reset();
    if (bloom_) bloom_.reset();

    if (sunShafts_) {
        sunShafts_->shutdown();
        sunShafts_.reset();
    }

    LOG_DEBUG("Renderer::shutdown - skySystem...");
    if (skySystem) {
        skySystem->shutdown();
        skySystem.reset();
    }

    // Individual sky components are owned by skySystem; just null the aliases
    skybox = nullptr;
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    if (precipitation_) {
        precipitation_->shutdown();
        precipitation_.reset();
    }
    if (shelter_) {
        shelter_->shutdown();
        shelter_.reset();
    }
    if (lensWeather_) {
        lensWeather_->shutdown();
        lensWeather_.reset();
    }

    if (lightning) {
        lightning->shutdown();
        lightning.reset();
    }

    if (swimEffects) {
        swimEffects->shutdown();
        swimEffects.reset();
    }

    if (footprintRenderer) {
        footprintRenderer->shutdown();
        footprintRenderer.reset();
    }

    characterEmitters_.reset();

    LOG_DEBUG("Renderer::shutdown - characterRenderer...");
    if (characterRenderer) {
        characterRenderer->shutdown();
        characterRenderer.reset();
    }

    // Shutdown AnimationController before renderers it references (§4.2)

    LOG_DEBUG("Renderer::shutdown - wmoRenderer...");
    if (wmoRenderer) {
        wmoRenderer->shutdown();
        wmoRenderer.reset();
    }

    if (grassRenderer_) {
        // The whole session in one line, at the end where the bounded log
        // cannot rotate it away before anyone reads it.
        if (grassRebuilds_ > 0) {
            LOG_INFO("Grass session: ", grassRebuilds_, " rebuilds, last population ",
                     grassLastCount_, " blades, ", grassProfiles_.size(),
                     " profiles derived, worst generate ",
                     static_cast<int>(grassWorstGenerateMs_), "ms");
        }
        grassRenderer_->shutdown();
        grassRenderer_.reset();
    }

    LOG_DEBUG("Renderer::shutdown - m2Renderer...");
    if (hizSystem_) {
        hizSystem_->shutdown();
        hizSystem_.reset();
    }
    if (m2Renderer) {
        m2Renderer->shutdown();
        m2Renderer.reset();
    }
    if (skyboxModelRenderer_) {
        skyboxModelRenderer_->shutdown();
        skyboxModelRenderer_.reset();
        skyLayers_.clear();
        loadedSkyModels_.clear();
    }

    // Cleanup selection circle + overlay resources
    if (overlaySystem_) {
        overlaySystem_->cleanup();
        overlaySystem_.reset();
    }

    // Shutdown post-process pipeline (FSR/FXAA/FSR2 resources) (§4.3)
    if (postProcessPipeline_) {
        postProcessPipeline_->shutdown();
        postProcessPipeline_.reset();
    }

    // Destroy render graph
    renderGraph_.reset();

    destroyPerFrameResources();
    // Shutdown has fenced the device; drain the last post-process/RT retirements
    // before the context destroys its allocator. No more frames will run.
    if (vkCtx) vkCtx->flushDeferredCleanup();

    camera.reset();

    LOG_INFO("Renderer shutdown");
}

void Renderer::registerComposite(const std::shared_ptr<OffscreenComposite>& composite) {
    if (!composite) return;
    const bool registered = std::any_of(offscreenComposites_.begin(), offscreenComposites_.end(),
        [&](const auto& weak) { return weak.lock() == composite; });
    if (!registered) offscreenComposites_.push_back(composite);
}

void Renderer::unregisterComposite(OffscreenComposite* composite) {
    std::erase_if(offscreenComposites_, [&](const auto& weak) {
        const auto owner = weak.lock();
        return !owner || owner.get() == composite;
    });
}

void Renderer::setWaterRefractionEnabled(bool /*enabled*/) {
    // Always on. Kept as a call rather than deleted because saved settings and
    // the CVar bridge still reach it, and a config written before this that says
    // 0 should not be able to turn it off again.
    if (waterRenderer) waterRenderer->setRefractionEnabled(true);
}
void Renderer::setMsaaSamples(VkSampleCountFlagBits samples) {
    if (!vkCtx) return;

    // Only a device that cannot resolve depth has to choose between the
    // temporal upscaler and multisampling; everywhere else both run. The
    // saved choice is kept either way and re-asserted when the upscaler goes
    // off, by the settings panel.
    if (postProcessPipeline_ && postProcessPipeline_->isFSR2Enabled() &&
        !vkCtx->isDepthResolveSupported() && samples > VK_SAMPLE_COUNT_1_BIT) {
        LOG_WARNING("Multisampling left off: the temporal upscaler is on and this device "
                    "cannot resolve depth");
        return;
    }

    // Clamp to device maximum
    VkSampleCountFlagBits maxSamples = vkCtx->getMaxUsableSampleCount();
    if (samples > maxSamples) samples = maxSamples;

    requestedMsaaSamples_ = samples;
}

void Renderer::applyMsaaChange() {
    VkSampleCountFlagBits samples = pendingMsaaSamples_;
    msaaChangePending_ = false;

    // Only where the upscaler cannot share a multisampled scene - a device
    // with no depth resolve - does a queued change have to fall back to 1x.
    if (samples > VK_SAMPLE_COUNT_1_BIT &&
        postProcessPipeline_ && postProcessPipeline_->isFsr2BlockingMsaa()) {
        samples = VK_SAMPLE_COUNT_1_BIT;
    }

    VkSampleCountFlagBits current = vkCtx->getMsaaSamples();
    if (samples == current) return;

    // Set new MSAA and recreate swapchain (render pass, depth, MSAA image, framebuffers)
    vkCtx->setMsaaSamples(samples);
    if (!vkCtx->recreateSwapchain(frameInputs_.drawableWidth, frameInputs_.drawableHeight)) {
        LOG_ERROR("MSAA change failed - reverting to 1x");
        vkCtx->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
        (void)vkCtx->recreateSwapchain(frameInputs_.drawableWidth, frameInputs_.drawableHeight);
    }

    // Recreate all sub-renderer pipelines (they embed sample count from render pass)
    if (terrainRenderer) terrainRenderer->recreatePipelines();
    if (grassRenderer_) grassRenderer_->recreatePipelines();
    if (distantTerrain_) distantTerrain_->recreatePipeline();
    if (waterRenderer) {
        waterRenderer->recreatePipelines();
        // Under MSAA the water draws single-sampled in its own pass, after the
        // scene has resolved - it is a large alpha-blended surface whose edges
        // MSAA does nothing for, and drawing it there also keeps it out of its
        // own refraction copy.
        waterRenderer->destroyWater1xResources();
        setupWater1xPass();
    }
    if (wmoRenderer) wmoRenderer->recreatePipelines();
    if (m2Renderer) m2Renderer->recreatePipelines();
    if (skyboxModelRenderer_) skyboxModelRenderer_->recreatePipelines();
    if (characterRenderer) characterRenderer->recreatePipelines();
    if (questMarkerRenderer) questMarkerRenderer->recreatePipelines();
    if (footprintRenderer) footprintRenderer->recreatePipelines();
    if (lightning) lightning->recreatePipelines();
    if (swimEffects) {
        syncSwimEffectsTargetPass();
        swimEffects->recreatePipelines();
    }
    if (precipitation_) {
        syncSwimEffectsTargetPass();
        precipitation_->recreatePipelines();
        precipitationDrawsWithWater_ = swimEffectsDrawWithWater_;
    }
    if (mountDust) mountDust->recreatePipelines();
    if (chargeEffect) chargeEffect->recreatePipelines();

    // Sky system sub-renderers
    if (skySystem) {
        if (auto* sb = skySystem->getSkybox()) sb->recreatePipelines();
        if (auto* sf = skySystem->getStarField()) sf->recreatePipelines();
        if (auto* bodies = skySystem->getBodies()) bodies->recreatePipelines();
        if (auto* cl = skySystem->getClouds()) cl->recreatePipelines();
        if (auto* lf = skySystem->getLensFlare()) lf->recreatePipelines();
    }

    if (minimap) {
        // After syncSwimEffectsTargetPass above, which is what decides the pass
        // this is built against.
        minimap->recreatePipelines();
    }

    // Resize HiZ pyramid (depth format/MSAA may have changed)
    if (hizSystem_) {
        auto ext = vkCtx->getSwapchainExtent();
        if (!hizSystem_->resize(ext.width, ext.height)) {
            LOG_WARNING("HiZ resize failed after MSAA change");
            if (m2Renderer) m2Renderer->setHiZSystem(nullptr);
            hizSystem_->shutdown();
            hizSystem_.reset();
        }
    }

    // Selection circle + overlay + FSR use lazy init, just destroy them
    if (overlaySystem_) overlaySystem_->recreatePipelines();
    if (postProcessPipeline_) postProcessPipeline_->destroyAllResources(); // Will be lazily recreated in beginFrame()

    // ImGui is deliberately not restarted here.
    //
    // It always initialises at one sample into the overlay pass, which is
    // itself always single-sampled and depends only on the swapchain format -
    // so a change of scene anti-aliasing does not change anything ImGui built.
    // Recreating the swapchain produces a new overlay pass handle, but a
    // structurally identical one, and Vulkan requires a pipeline's render pass
    // to be compatible rather than the same object.
    //
    // Tearing the backend down destroyed its descriptor pool, and every UI
    // texture in the client - item and spell icons, raid icons, the talent
    // background, the world map layers, the widget renderer - holds a
    // descriptor set allocated from it. Nothing was told, so the next frame
    // drew with freed descriptors and the GPU was reset: the log shows the
    // swapchain and pipelines rebuilt, then the fence wait failing with
    // VK_ERROR_DEVICE_LOST a fraction of a second later. Applying a saved
    // anti-aliasing setting at startup made that look like a crash on launch.

}

FrameInputs Renderer::extractFrameInputs() {
    CpuStageLaps extraction(vkCtx ? &vkCtx->gameCpuStages() : nullptr, "extract");
    std::optional<CpuStage> extractStage;
    if (vkCtx) extractStage.emplace(vkCtx->gameCpuStages(), "Present", "extract scene");
    if (std::getenv("WOWEE_SKY_BACKGROUND_PROBE") && updateSkyboxLayers()) {
        skyboxModelRenderer_->setSkyTimeOfDay(lightingManager->getTimeOfDayHours());
        skyboxModelRenderer_->update(0.016f, camera->getViewMatrix(), camera->getProjectionMatrix());
    }
    updateGrassPopulation();
    extraction.lap("grass population");
    FrameInputs in;
    in.worldEpoch = worldEpoch_;
    in.capture = extractCaptureRequest();
    in.screenshots.swap(screenshotRequests_);
    if (overlaySystem_) in.selection = overlaySystem_->extractSelection(*collision_);
    if (camera) in.camera = *camera;
    in.camera.clearJitter();
    in.characterPosition = characterPosition;
    in.characterYaw = characterYaw;
    in.characterInstance = characterInstanceId;
    in.environment = environment_;
    in.viewer = viewerMotion_;
    in.time = globalTime;
    in.deltaTime = lastDeltaTime_;
    in.hasLighting = lightingManager != nullptr;
    if (lightingManager) {
        in.lighting = lightingManager->getLightingParams();
        in.weatherLight = *weatherLight();
        in.timeOfDay = lightingManager->getTimeOfDayHours();
    }
    in.weather = weather_.mix();
    in.wind = wind_.gpu();
    in.windDirection = wind_.state().direction;
    in.lensDrops = lensDrops_;
    in.lensWind = lensWindOnScreen_;
    in.sunGlare = sunGlare_;
    in.sunGlareSky = sunGlareSky_;
    in.moonGlare = moonGlare_;
    in.viewDistance = viewDistance_;
    in.shadowDistance = shadowDistance_;
    in.shadows = shadowsEnabled;
    in.shadowMatrix = computeLightSpaceMatrix(in, in.shadowCenter);
    in.worldInView = worldInView_;
    in.terrainEnabled = terrainEnabled;
    in.distantTerrainReady = distantTerrain_ && distantTerrain_->ready();
    in.drawSkyModels = skyboxModelRenderer_ && !skyLayers_.empty();
    in.hideProceduralSky = skyModelsHideProceduralSky();
    in.sunShafts = sunShaftsEnabled_;
    in.sharpStars = sharpStars_;
    in.minimapEnabled = minimap && minimap->isEnabled();
    if (surface_) {
        in.drawableWidth = surface_->getDrawableWidth();
        in.drawableHeight = surface_->getDrawableHeight();
        in.windowWidth = surface_->getWidth();
        in.windowHeight = surface_->getHeight();
    }
    in.rtMode = requestedRtMode_;
    in.rtDistance = requestedRtDistance_;
    in.fogQuality = requestedFogQuality_;
    in.msaaSamples = static_cast<uint32_t>(requestedMsaaSamples_);
    if (postProcessPipeline_) in.postProcess = postProcessPipeline_->requestedSettings();
    in.fogDensity = volumetricFogDensity_;
    in.fogTargetExtinction = volumetricFogExtinction();
    in.fogGround = characterPosition.z;
    in.fogStrength = lightingManager ? lightingManager->getFogStrength() : 1.f;
    if (const auto h = collision_->terrainHeight(characterPosition.x, characterPosition.y))
        in.fogGround = std::min(in.fogGround, *h);
    in.cameraWaterDepth = cameraWaterDepth();
    if (camera) {
        const auto eye = camera->getPosition();
        in.reflectionWaterHeight = liquidScene_.dominantWaterHeight(eye);
        in.waterHeight = liquidScene_.nearestHeightAt(eye.x, eye.y, eye.z, 400.0f);
        in.waterIsWmo = liquidScene_.isWmoLiquidAt(eye.x, eye.y);
        if (const auto type = liquidScene_.typeAt(eye.x, eye.y))
            in.canal = *type == 5 || *type == 13 || *type == 17;
        if (waterRenderer)
            in.seaWaterHeight = waterRenderer->highestOpenWaterNear(glm::vec2(eye), SeaBedMap::kHalfExtent);
        if (m2Renderer) m2Renderer->gatherLocalLights(eye, in.localLights);
        if (characterRenderer) characterRenderer->gatherLocalLights(in.camera, in.localLights);
    }
    extraction.lap("globals, collision and local lights");
    in.shelterRevision = wmoRenderer ? wmoRenderer->contentRevision() : 0;
    in.shelterRevision = in.shelterRevision * 1000003u + (wmoRenderer ? wmoRenderer->getInstanceCount() : 0);
    in.shelterRevision = in.shelterRevision * 1000003u + (m2Renderer ? m2Renderer->getInstanceCount() : 0);
    in.shelterRevision = in.shelterRevision * 1000003u + (terrainRenderer ? terrainRenderer->getChunkCount() : 0);
    // Player motion, consumed by water ripples and by the foliage brush in
    // m2.vert. Horizontal speed only: a fall shouldn't read as running.
    {
        const float dt = std::max(lastDeltaTime_, 0.0f);
        if (!playerMotionTracked_) {
            prevPlayerPos_ = characterPosition;
            playerWakePos_ = characterPosition;
            playerMotionTracked_ = true;
        }
        if (dt > 0.0f) {
            const glm::vec2 step(characterPosition.x - prevPlayerPos_.x,
                                 characterPosition.y - prevPlayerPos_.y);
            // A teleport is not a sprint. Anything past a gallop is discarded
            // rather than smoothed, or one map change flattens a whole field.
            constexpr float MAX_TRACKED_SPEED = 30.0f;
            const float rawSpeed = glm::length(step) / dt;
            playerSpeed_ = (rawSpeed > MAX_TRACKED_SPEED) ? 0.0f
                                                          : glm::mix(playerSpeed_, rawSpeed, 0.25f);

            // Exponential chase, framerate independent.
            constexpr float WAKE_TIME_CONSTANT = 0.30f;  // seconds of springback
            const float k = 1.0f - std::exp(-dt / WAKE_TIME_CONSTANT);
            playerWakePos_ += (characterPosition - playerWakePos_) * k;
            if (rawSpeed > MAX_TRACKED_SPEED) playerWakePos_ = characterPosition;
        }
        prevPlayerPos_ = characterPosition;
    }
    in.playerPos = glm::vec4(characterPosition, playerSpeed_);
    in.playerWake = glm::vec4(playerWakePos_, 0.0f);
    auto scene = std::make_shared<FrameScene>();
    if (lightning) scene->lightning = lightning->extract();
    scene->sequence = ++extractedSceneSequence_;
    if (vkCtx) scene->resources = vkCtx->holdSnapshotResources();
    extraction.lap("motion and resource lease");
    if (terrainRenderer) {
        terrainRenderer->setViewDistance(in.distantTerrainReady ? std::min(in.viewDistance,kDistantTerrainEnd) : in.viewDistance);
        terrainRenderer->setLodViewportHeight(sceneRenderExtent().height);
        scene->terrain = terrainRenderer->extract(in.camera, in.localLights);
    }
    if(distantTerrain_ && terrainRenderer && in.terrainEnabled)
        scene->distantTerrain=distantTerrain_->extract(in.camera,in.viewDistance,terrainRenderer->detailedCoverage());
    extraction.lap("terrain");
    if (waterRenderer) {
        scene->water = waterRenderer->extract(in.camera);
        if (in.hasLighting) {
            const auto sun = sky_celestial::sunDisc(in.timeOfDay / 24.0f);
            const float visible = sky_celestial::discHorizonAlpha(sun, 0.0f) * in.sunGlareSky *
                                  sky_celestial::weatherFade(in.weather.strength());
            scene->water.water.reflectionSun = glm::vec4(sun.direction, visible);
            scene->water.water.reflectionSunColor = glm::vec4(in.weatherLight.sunColor, 0.0f);
        }
    }
    extraction.lap("water");
    if (wmoRenderer) {
        wmoRenderer->setViewDistance(in.viewDistance);
        scene->buildings = wmoRenderer->extract(in.camera, in.characterPosition, in.localLights, scene->water.inView);
    }
    extraction.lap("WMO");
    if (characterRenderer) scene->characters = characterRenderer->extract(in.camera, in.localLights);
    extraction.lap("characters");
    if (m2Renderer) {
        m2Renderer->setViewDistance(in.viewDistance);
        const auto shelterFrame = overheadFrameFor(glm::vec2(in.camera.getPosition()),
            in.camera.getPosition().z + ShelterMap::kAbove, ShelterMap::kHalfExtent,
            ShelterMap::kRange, ShelterMap::kTexels);
        const glm::mat4 shelterMatrix = shelterFrame.viewProjection();
        scene->models = m2Renderer->extract(in.camera, in.localLights, in.time, in.shadowMatrix,
            in.shadowDistance * 1.35f, in.weather.any() ? &shelterMatrix : nullptr,
            ShelterMap::kHalfExtent * 1.415f,
            scene->lightning.lightColor.b > 0.0f ? &scene->lightning.lightMatrix : nullptr, 550.0f);
    }
    extraction.lap("M2");
    if (minimap && in.minimapEnabled) {
        const auto centre = in.viewer.thirdPerson ? in.characterPosition : in.camera.getPosition();
        scene->minimap = minimap->extract(in.camera, centre, in.windowWidth, in.windowHeight,
                                          glm::radians(in.characterYaw), true);
    }
    extraction.lap("minimap");
    if (skyboxModelRenderer_) {
        skyboxModelRenderer_->setSuppressBakedStars(in.sharpStars);
        scene->skyModels = skyboxModelRenderer_->extract(in.camera, in.localLights, in.time, glm::mat4(0.0f), 1.0f);
    }
    extraction.lap("sky models");
    if (grassRenderer_) scene->grass = grassRenderer_->extract();
    if (questMarkerRenderer) scene->markers = questMarkerRenderer->extract(in.camera, in.time);
    if (footprintRenderer) scene->footprints = footprintRenderer->extract(in.camera);
    if (skySystem) {
        scene->sky = skySystem->extract();
        scene->sky.proceduralStars = in.sharpStars;
    }
    if (mountDust) scene->dust = mountDust->extract();
    if (swimEffects) scene->swim = swimEffects->extract();
    std::erase_if(offscreenComposites_, [](const auto& weak) { return weak.expired(); });
    for (const auto& weak : offscreenComposites_) {
        if (auto owner = weak.lock()) {
            auto request = owner->extractComposite();
            if (request) scene->composites.push_back(std::move(request));
        }
    }
    if (chargeEffect) scene->charge = chargeEffect->extract();
    if (precipitation_) scene->precipitation = precipitation_->extract();
    extraction.lap("effects and composites");
    if (in.rtMode != 0) {
        if (wmoRenderer) wmoRenderer->syncRtScene();
        if (m2Renderer) m2Renderer->syncRtScene();
    }
    extraction.lap("RT source sync");
    if (rtScene_) {
        RtRegion region;
        region.enabled = in.rtMode != 0;
        region.centre = in.camera.getPosition();
        region.distance = in.rtDistance;
        region.indirectLength = in.rtMode >= 3 ? kRtGiLength : in.rtMode >= 2 ? kRtAoRadius : 0.0f;
        region.sunDirection = in.hasLighting ? -in.lighting.directionalDir : glm::vec3(0, 0, 1);
        region.sunlight = region.sunDirection.z > -0.05f;
        scene->rtChanges = rtScene_->extractChanges(region);
    }
    extraction.lap("RT region changes");
    in.scene = std::move(scene);

    return in;
}

void Renderer::beginFrame(const FrameInputs& inputs) {
    frameInputs_ = inputs;
    recordingResult_ = FrameResults();
    recordingResult_.recording = ++recordingSequence_;
    recordingResult_.gameFrame = inputs.scene ? inputs.scene->sequence : 0;
    recordingGpuFrame_ = vkCtx ? vkCtx->frameNumber() : 0;
    worldDrawnThisFrame_ = false;
    renderCamera_ = inputs.camera;
    ZoneScopedN("Renderer::beginFrame");
    if (vkCtx) vkCtx->cpuStages().clear();
    if (!vkCtx) return;
    if (vkCtx->isDeviceLost()) return;
    if (!inputs.drawableWidth || !inputs.drawableHeight) return;
    const auto publishedExtent = vkCtx->getSwapchainExtent();
    if (inputs.drawableWidth != publishedExtent.width || inputs.drawableHeight != publishedExtent.height)
        vkCtx->markSwapchainDirty(); // also catches a resize arriving during recreation
    if (inputs.scene && inputs.scene->resources) inputs.scene->resources->usedByFrame(vkCtx->frameNumber());
    if (inputs.scene) {
        if (characterRenderer) characterRenderer->useFrame(inputs.scene->characters);
        if (wmoRenderer) wmoRenderer->useFrame(inputs.scene->buildings);
        if (m2Renderer) m2Renderer->useFrame(inputs.scene->models);
        if (skyboxModelRenderer_) skyboxModelRenderer_->useFrame(inputs.scene->skyModels);
    }
    if (inputs.scene && inputs.scene->sequence != recordedSceneSequence_) {
        if (rtScene_) rtScene_->applyChanges(inputs.scene->rtChanges);
        recordedSceneSequence_ = inputs.scene->sequence;
    }
    // The renderer's frame starts here unless update() already began it;
    // its parts are read after endFrame.
    CpuStageTimes& stages = vkCtx->cpuStages();

    // The ablation's clock. Top of one frame to the top of the next is the
    // frame's whole wall time, which is the number a pass has to move to be
    // worth anything - a pass that gets cheaper while the frame does not has
    // not been paid for.
    if (passAblation_) {
        const auto now = std::chrono::steady_clock::now();
        // Only frames that drew the world. A frame of character select costs
        // five milliseconds and a frame with a zone streaming into it costs a
        // hundred and twenty, and the first run of this walked its baseline
        // through the former and its terrain phase through the latter - it
        // reported that terrain was worth minus 110 milliseconds.
        if (worldDrawnLastFrame_ && lastFrameStart_.time_since_epoch().count() != 0) {
            const bool wasSettling = passAblation_->settling();
            const AblationPass before = passAblation_->current();
            passAblation_->frame(std::chrono::duration<double, std::milli>(
                now - lastFrameStart_).count());
            // Say where it has got to. A run is the better part of a minute of
            // standing still and the first one silently went eight phases deep
            // before it was quit, with no way to tell it had started.
            if (passAblation_->running() &&
                (passAblation_->current() != before || (wasSettling && !passAblation_->settling()))) {
                LOG_WARNING("Pass ablation ", passAblation_->phaseNumber(), "/",
                            PassAblation::phaseCount(), ": ",
                            ablationPassName(passAblation_->current()),
                            passAblation_->current() == AblationPass::None ? "" : " off");
            }
        }
        lastFrameStart_ = now;
        worldDrawnLastFrame_ = false;
        if (!passAblation_->running() && !passAblationReported_) {
            passAblationReported_ = true;
            LOG_WARNING(passAblation_->report());
        }
    }

    worldDrawnThisFrame_ = false;

    if (rtLighting_) rtLighting_->setMode(static_cast<RtLighting::Mode>(inputs.rtMode));
    if (volumetricFog_)
        volumetricFog_->setQuality(static_cast<VolumetricFog::Quality>(inputs.fogQuality));
    pendingMsaaSamples_ = static_cast<VkSampleCountFlagBits>(inputs.msaaSamples);
    if (postProcessPipeline_) {
        const auto req = postProcessPipeline_->applyFrameSettings(inputs.postProcess);
        if (req.requested) pendingMsaaSamples_ = req.samples;
    }
    if (inputs.postProcess.fsr ||
        (inputs.postProcess.fsr2 && !vkCtx->isDepthResolveSupported()))
        pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    msaaChangePending_ = pendingMsaaSamples_ != vkCtx->getMsaaSamples();

    // Settings applied between frames, the targets they size, and the upload
    // batches the GPU has finished with.
    std::optional<CpuStage> targetsStage(std::in_place, stages, "beginFrame", "targets and uploads");

    // Apply deferred MSAA change between frames (before any rendering state is used)
    if (msaaChangePending_) {
        applyMsaaChange();
        // The rebuild destroys and remakes the swapchain, every render pass and
        // every pipeline. The frame slots are left mid-cycle by it, and the
        // next frame would reset a fence and re-record a command buffer the
        // GPU has not finished with - which is what validation reports and the
        // driver answers by losing the device.
        if (vkCtx) vkCtx->resetFrameSyncState();
    }

    // A fog quality change builds or frees its volumes, which the per-frame
    // sets bind - so between frames, before this one's set is used.
    if (volumetricFog_) (void)volumetricFog_->applyPendingQuality();

    // Retire finished upload batches every frame.
    //
    // This was polled only from the terrain manager, so batches submitted by
    // anything else retired only while terrain happened to be streaming. A
    // rebuild reported 1423 submitted against 1241 retired - 182 outstanding,
    // each holding a fence, a command buffer and its staging buffers. With
    // FrameXML uploading hundreds of textures the backlog is much larger than
    // it was, and nothing bounded it.
    if (vkCtx) vkCtx->pollUploadBatches();

    // Post-process resource management (§4.3 - delegates to PostProcessPipeline)
    if (postProcessPipeline_) postProcessPipeline_->manageResources();

    // With MSAA, the water's pass over that target, rebuilt if the target was.
    updateOffscreenWater1xFramebuffer();

    // The target that call may have just created or destroyed decides where
    // the water draws, and with it where the spray and the minimap go. Asked
    // again here, after the answer can have changed, rather than only at the
    // anti-aliasing rebuild that runs before the target exists.
    refreshSwimEffectsPass();

    // Handle swapchain recreation if needed
    if (vkCtx->isSwapchainDirty()) {
        // Skip recreation while window is minimized (0×0 extent is a Vulkan spec violation)
        // Pixels, like the extent below: a window with a surface behind it is
        // never rebuilt from the size the desktop places it at.
        if (inputs.drawableWidth == 0 || inputs.drawableHeight == 0) return;
        // A swapchain or a frame target that could not be made skips the
        // frame: the context has said why, and tries again next frame. The
        // rebuilds below would size themselves from a swapchain that is not
        // there, and recording against it is what crashed the driver.
        if (!vkCtx->recreateSwapchain(frameInputs_.drawableWidth, frameInputs_.drawableHeight)) return;
        // Rebuild water resources that reference swapchain extent/views
        if (waterRenderer) {
            waterRenderer->recreatePipelines();
            waterRenderer->destroyWater1xResources();
            setupWater1xPass();
        }
        // Recreate post-process resources for new swapchain dimensions
        if (postProcessPipeline_) postProcessPipeline_->handleSwapchainResize();
        // Resize HiZ depth pyramid for new swapchain dimensions
        if (hizSystem_) {
            auto ext = vkCtx->getSwapchainExtent();
            if (!hizSystem_->resize(ext.width, ext.height)) {
                LOG_WARNING("HiZ resize failed - disabling occlusion culling");
                if (m2Renderer) m2Renderer->setHiZSystem(nullptr);
                hizSystem_->shutdown();
                hizSystem_.reset();
            }
        }
    }

    // Retire replaced images; each consumer set is rewritten after its fence.
    if (rtLighting_) {
        rtLighting_->setDistance(inputs.rtDistance);
        rtLighting_->prepare(sceneRenderExtent());
    }
    targetsStage.reset();

    rtRecordedThisFrame_ = false;

    // Acquire swapchain image and begin command buffer. Its waits are timed
    // inside: for the GPU, for the image, for uploads.
    currentCmd = vkCtx->beginFrame(currentImageIndex);
    if (currentCmd == VK_NULL_HANDLE) {
        // Swapchain out of date, will retry next frame
        return;
    }

    {
        CpuStage stage(stages, "beginFrame", "frame bindings");
        // This slot's fence has just been waited on, so a frame it copied for the
        // recording two frames ago is complete.
        collectRecordedFrame();
        // And so is the probe of the shelter round the camera it took.
        if (shelter_) shelter_->collect(vkCtx->getCurrentFrame());
        // The sun's and moon's occlusion answers from this slot's last frame, read
        // now its fence is passed, and its queries reset before any pass begins.
        if (skySystem && skySystem->getBodies()) {
            skySystem->getBodies()->beginFrameQueries(currentCmd, vkCtx->getCurrentFrame());
        }
        writeRtLightingBindings(vkCtx->getCurrentFrame());
        writeFogVolumeBindings(vkCtx->getCurrentFrame());

        // FSR2 jitter pattern (§4.3 - delegates to PostProcessPipeline)
        if (postProcessPipeline_ && frameCamera()) postProcessPipeline_->applyJitter(&renderCamera_);
    }
    // The characters' bones into this slot's buffers, once, now that its
    // fence is passed and before the shadow maps draw them: every pass after
    // this only binds them.
    if (characterRenderer) {
        CpuStage stage(stages, "beginFrame", "prepare characters");
        characterRenderer->prepareRender(vkCtx->getCurrentFrame());
    }

    {
        CpuStage stage(stages, "beginFrame", "per-frame UBO");
        // Compute fresh shadow matrix BEFORE UBO update so shaders get current-frame data.
        lightSpaceMatrix = frameInputs_.shadowMatrix;
        shadowCenter = frameInputs_.shadowCenter;

        // Update per-frame UBO with current camera/lighting state
        updatePerFrameUBO();
    }

    // Prepare and update sets before any pre-pass binds them (volumetric fog
    // uses the world set too). Updating an already recorded set invalidates it.
    const uint32_t skySlot = vkCtx->getCurrentFrame();
    std::optional<CpuStage> skySetStage(std::in_place, stages, "beginFrame", "sky set");
    if (skyBackground_ && skyBackground_->prepare(*vkCtx, skySlot, sceneRenderExtent())) {
        const uint32_t slot = skySlot;
        // The sky uses the same light, shadows and fog volume, but a separate set
        // whose sky sampler stays neutral: never sample the attachment being drawn.
        VkCopyDescriptorSet copies[5]{};
        for (uint32_t binding = 0; binding < 5; ++binding) {
            copies[binding].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            copies[binding].srcSet = perFrameDescSets[slot];
            copies[binding].srcBinding = binding;
            copies[binding].dstSet = skyPerFrameDescSets[slot];
            copies[binding].dstBinding = binding;
            copies[binding].descriptorCount = 1;
        }
        VkDescriptorImageInfo image{};
        image.imageView = skyBackground_->view(slot);
        image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = perFrameDescSets[slot];
        write.dstBinding = 5;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 5, copies);
    }
    skySetStage.reset();

    // ── Early compute: M2 frustum culling ──
    // beginFrame() has already waited for this frame slot's previous fence, so
    // its mapped visibility output is complete and safe for the CPU to reuse.
    // Read/invalidate that completed output, then record the next cull dispatch
    // directly into the normal frame command buffer. The old path submitted a
    // separate command buffer and synchronously waited on a fence every frame,
    // serializing CPU and GPU work solely to obtain same-frame cull results.
    if (m2Renderer && frameCamera() && vkCtx) {
        CpuStage stage(stages, "beginFrame", "M2 cull");
        GpuPass pass(*vkCtx, currentCmd, "M2 cull");
        uint32_t frame = vkCtx->getCurrentFrame();
        m2Renderer->invalidateCullOutput(frame);
        m2Renderer->dispatchCullCompute(currentCmd, frame, *frameCamera());
    }

    // Grass culls here too, for the same reason: a dispatch has to be recorded
    // outside a render pass. Unlike the M2 path nothing reads the result back -
    // the count it produces is consumed by the indirect draw on the GPU.
    if (grassRenderer_ && frameCamera() && vkCtx) {
        CpuStage stage(stages, "beginFrame", "grass");
        GpuPass pass(*vkCtx, currentCmd, "grass cull");
        grassRenderer_->reportCullResult();
        grassRenderer_->dispatchCull(currentCmd, vkCtx->getCurrentFrame(), *frameCamera(),
                                     frameInputs_.characterPosition, frameInputs_.scene->grass);
    }

    // --- Off-screen pre-passes ---
    // Build frame graph: registers pre-passes as graph nodes with dependencies.
    // compile() topologically sorts; execute() runs them with auto barriers.
    {
        CpuStage stage(stages, "beginFrame", "graph build");
        buildFrameGraph();
    }
    if (renderGraph_) {
        // Each pass under "graph", by its name.
        CpuStage stage(stages, "beginFrame", "graph");
        renderGraph_->execute(currentCmd, *vkCtx, "beginFrame/graph");
    }

    // The real atmosphere is ready after the fog pre-pass. Capture it before
    // the main pass so its full-resolution MSAA attachments are cleared once.
    const bool skyProbe = std::getenv("WOWEE_SKY_BACKGROUND_PROBE") != nullptr;
    const bool skipSkyCapture = std::getenv("WOWEE_SKIP_SKY") ||
        std::getenv("WOWEE_SKIP_ALL_RENDER") ||
        (passAblation_ && passAblation_->skip(AblationPass::Sky));
    if (skySystem && frameCamera() && (frameInputs_.worldInView || skyProbe) && !skipSkyCapture) {
        CpuStage stage(stages, "beginFrame", "sky capture");
        GpuPass pass(*vkCtx, currentCmd, "sky capture");
        const bool authored = frameInputs_.drawSkyModels;
        const float timeOfDay = frameInputs_.hasLighting ? frameInputs_.timeOfDay : 12.0f;
        renderSkyBackground(timeOfDay, authored, frameInputs_.hideProceduralSky);
        if (skyProbe) {
            static bool logged = false;
            if (!logged) {
                LOG_INFO("Sky background GPU probe: captured current sky, authored=", authored,
                         ", MSAA=", int(vkCtx->getMsaaSamples()));
                logged = true;
            }
        }
    }

    // --- Begin render pass ---
    // Select framebuffer: PP off-screen target or swapchain (§4.3 - PostProcessPipeline)
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = vkCtx->getImGuiRenderPass();

    VkExtent2D renderExtent;
    VkFramebuffer ppFB = postProcessPipeline_ ? postProcessPipeline_->getSceneFramebuffer() : VK_NULL_HANDLE;
    if (ppFB != VK_NULL_HANDLE) {
        rpInfo.framebuffer = ppFB;
        renderExtent = postProcessPipeline_->getSceneRenderExtent();
    } else {
        rpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[currentImageIndex];
        renderExtent = vkCtx->getSwapchainExtent();
    }

    rpInfo.renderArea.offset = {.x = 0, .y = 0};
    rpInfo.renderArea.extent = renderExtent;

    // Clear values must match attachment count: 2 (no MSAA), 3 (MSAA), or 4 (MSAA+depth resolve)
    VkClearValue clearValues[4]{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {.depth = 1.0f, .stencil = 0};
    clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[3].depthStencil = {.depth = 1.0f, .stencil = 0};
    bool msaaOn = (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT);
    if (msaaOn) {
        bool depthRes = (vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE);
        rpInfo.clearValueCount = depthRes ? 4 : 3;
    } else {
        rpInfo.clearValueCount = 2;
    }
    rpInfo.pClearValues = clearValues;

    // Cache render pass state for secondary command buffer inheritance
    activeRenderPass_ = rpInfo.renderPass;
    activeFramebuffer_ = rpInfo.framebuffer;
    activeRenderExtent_ = renderExtent;

    VkSubpassContents subpassMode = parallelRecordingEnabled_
        ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(currentCmd, &rpInfo, subpassMode);
    // The pass's start - clearing its colour and depth, every sample of them
    // under MSAA - ends here. Without this mark it was read as part of the
    // first pass drawn in it, terrain.
    if (parallelRecordingEnabled_) recordSecondaryMark(0, "scene pass begin");
    else vkCtx->gpuMark(currentCmd, "scene pass begin");

    if (!parallelRecordingEnabled_) {
        // Fallback: set dynamic viewport and scissor on primary (inline mode)
        VkViewport viewport{};
        viewport.width = static_cast<float>(renderExtent.width);
        viewport.height = static_cast<float>(renderExtent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = renderExtent;
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);
    }

}

void Renderer::endFrame() {
    const auto recordingStart = std::chrono::steady_clock::now();
    ZoneScopedN("Renderer::endFrame");
    const auto releaseInputs = [this] {
        // Recording is complete. Keep GPU lifetimes through retirement rather
        // than retaining an extra completed CPU snapshot beside the two slots.
        frameInputs_.scene.reset();
        frameInputs_.interface.reset();
        frameInputs_.auxiliaryInterface = {};
        frameInputs_.capture = FrameCaptureRequest();
        frameInputs_.screenshots.clear();
        if (characterRenderer) characterRenderer->useFrame({});
        if (wmoRenderer) wmoRenderer->useFrame({});
        if (m2Renderer) m2Renderer->useFrame({});
        if (skyboxModelRenderer_) skyboxModelRenderer_->useFrame({});
    };
    if (!vkCtx || currentCmd == VK_NULL_HANDLE) { publishFrameResults(); releaseInputs(); return; }

    logViewDistanceDiag();

    // Post-process execution (§4.3 - delegates to PostProcessPipeline). Whether
    // it swapped the scene pass for an INLINE one no longer matters to the
    // caller: the UI is drawn in the overlay pass, which this function opens
    // itself once whichever pass is current has been closed.
    CpuStageTimes& stages = vkCtx->cpuStages();
    if (postProcessPipeline_) {
        CpuStage stage(stages, "endFrame", "post-process");
        postProcessPipeline_->executePostProcessing(
            currentCmd, currentImageIndex, frameCamera(), frameInputs_.deltaTime);
    }

    // The scene is complete: close its pass so the water refraction copy can run
    // (a copy is illegal inside a render pass), then draw the UI in the overlay
    // pass. Capturing after the UI instead is what refracted the interface into
    // the water. The overlay pass is single-sampled and colour-only, which is
    // also why the UI costs the same here whatever MSAA the scene uses.
    vkCmdEndRenderPass(currentCmd);
    vkCtx->gpuMark(currentCmd, "post-process");

    // Only when water could not be moved out of the scene pass (MSAA). Otherwise
    // renderWorld already took the copy at the one point in the frame where the
    // scene is finished but the water is not yet over it; copying again here
    // would replace that with an image containing the water.
    if (!waterDrawsInContinuePass() && !waterAblated()
        && waterRenderer && waterRenderer->isRefractionEnabled() && frameInputs_.scene->water.inView
        && currentImageIndex < vkCtx->getSwapchainImages().size()) {
        waterRenderer->captureSceneHistory(
            currentCmd,
            vkCtx->getSwapchainImages()[currentImageIndex],
            vkCtx->getDepthCopySourceImage(),
            vkCtx->getSwapchainExtent(),
            vkCtx->isDepthCopySourceMsaa(),
            vkCtx->getCurrentFrame());
    }

    // The picture is finished and out of every pass that drew it: the one
    // point where the shafts and the lens can copy it down, before the
    // overlay pass opens.
    if (bloom_) {
        GpuPass pass(*vkCtx, currentCmd, "bloom filter");
        const auto& images = vkCtx->getSwapchainImages();
        bloom_->record(currentCmd, vkCtx->getCurrentFrame(),
            worldDrawnThisFrame_ && currentImageIndex < images.size() ? images[currentImageIndex] : VK_NULL_HANDLE,
            vkCtx->getSwapchainExtent(), frameInputs_.postProcess.bloom);
    }
    recordSunShafts();
    recordLensWeather();

    const auto& overlayFbs = vkCtx->getOverlayFramebuffers();
    if (vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE && currentImageIndex < overlayFbs.size()) {
        CpuStage stage(stages, "endFrame", "interface");
        GpuPass pass(*vkCtx, currentCmd, "interface");
        VkRenderPassBeginInfo overlayRp{};
        overlayRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        overlayRp.renderPass = vkCtx->getOverlayRenderPass();
        overlayRp.framebuffer = overlayFbs[currentImageIndex];
        overlayRp.renderArea.extent = vkCtx->getSwapchainExtent();
        vkCmdBeginRenderPass(currentCmd, &overlayRp, VK_SUBPASS_CONTENTS_INLINE);

        VkExtent2D ext = vkCtx->getSwapchainExtent();
        VkViewport vp{};
        vp.width = static_cast<float>(ext.width);
        vp.height = static_cast<float>(ext.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = ext;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        // Bloom is derived from the world alone, never from the interface.
        if (bloom_) {
            GpuScope glow(*vkCtx, currentCmd, "bloom composite");
            bloom_->composite(currentCmd, vkCtx->getCurrentFrame());
        }
        // Under the interface, over everything else.
        if (sunShafts_) sunShafts_->composite(currentCmd, vkCtx->getCurrentFrame());
        // What the weather left on the lens, over the shafts as over the rest.
        if (lensWeather_) lensWeather_->composite(currentCmd, vkCtx->getCurrentFrame());

        // The interface's pipelines are built against the overlay pass, so it
        // always records inline here rather than into a scene-pass secondary.
        if (frameInputs_.interface) interfaceRecorder_.record(*vkCtx, currentCmd,
            vkCtx->getOverlayRenderPass(), vkCtx->getCurrentFrame(), *frameInputs_.interface);
        vkCmdEndRenderPass(currentCmd);
    } else {
        LOG_ERROR("Overlay render pass missing - UI not drawn this frame");
    }

    // Water now renders in the main pass (renderWorld), no separate 1x pass needed.

    if (frameInputs_.auxiliaryInterface)
        frameInputs_.auxiliaryInterface(currentCmd, vkCtx->getCurrentFrame());

    // Last, so the recording holds everything the player sees - and then the
    // recording dot, which it does not.
    recordScreenCapture();

    // Submit and present
    vkCtx->endFrame(currentCmd, currentImageIndex);
    currentCmd = VK_NULL_HANDLE;
    vkCtx->cpuStages().add("Render", "endFrame", std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - recordingStart).count());
    publishFrameResults();
    releaseInputs();
}

void Renderer::setCharacterFollow(uint32_t instanceId) {
    characterInstanceId = instanceId;
}

bool Renderer::updateSkyboxLayers() {
    // Which skybox models a place uses is Light.dbc's answer, not a map id:
    // LightParams names a LightSkybox row and LightSkybox names the model.
    // LightingManager walks that for the lights around the player and says
    // how much of each is up; this keeps one instance per model and fades it
    // by that weight, so crossing from one zone's sky to another's is a blend
    // and not a swap. WOWEE_NO_SKY_M2=1 draws the procedural sky alone.
    static const bool noSkyM2 = std::getenv("WOWEE_NO_SKY_M2") != nullptr;
    if (noSkyM2) return false;
    if (!skyboxModelRenderer_ || !lightingManager || !cachedAssetManager || !camera) {
        return false;
    }

    auto normalized = [](std::string p) {
        std::replace(p.begin(), p.end(), '/', '\\');
        return p;
    };
    const auto& layers = lightingManager->getSkyboxLayers();

    // Faded out and no longer wanted.
    std::erase_if(skyLayers_, [&](const SkyLayerInstance& sky) {
        const bool wanted = std::any_of(layers.begin(), layers.end(), [&](const auto& l) {
            return l.id == sky.skyboxId;
        });
        if (!wanted) skyboxModelRenderer_->removeInstance(sky.instanceId);
        return !wanted;
    });

    // One instance per LightSkybox row, as WoW.exe draws one per slot: two
    // rows naming the same model each draw it at their own weight. The model
    // itself is uploaded once, by path.
    for (const auto& layer : layers) {
        auto it = std::find_if(skyLayers_.begin(), skyLayers_.end(),
                               [&](const SkyLayerInstance& s) { return s.skyboxId == layer.id; });
        if (it == skyLayers_.end()) {
            const uint32_t modelId = loadSkyboxModel(normalized(layer.path));
            if (modelId == 0) continue;
            const uint32_t instanceId = skyboxModelRenderer_->createInstance(
                modelId, camera->getPosition(), glm::vec3(0.0f), 1.0f);
            if (instanceId == 0) continue;
            skyboxModelRenderer_->setSkipCollision(instanceId, true);
            skyLayers_.push_back({.skyboxId = layer.id, .instanceId = instanceId});
            it = std::prev(skyLayers_.end());
        }
        skyboxModelRenderer_->setInstanceFade(it->instanceId, layer.weight);
        skyboxModelRenderer_->setInstancePosition(it->instanceId, camera->getPosition());
    }
    return !skyLayers_.empty();
}

bool Renderer::skyModelsHideProceduralSky() const {
    if (!skyboxModelRenderer_ || !lightingManager) return false;
    return lightingManager->skyModelsHideProceduralSky([&](uint32_t skyboxId) {
        return std::any_of(skyLayers_.begin(), skyLayers_.end(),
                           [&](const SkyLayerInstance& s) { return s.skyboxId == skyboxId; });
    });
}

uint32_t Renderer::loadSkyboxModel(const std::string& path) {
    if (auto it = loadedSkyModels_.find(path); it != loadedSkyModels_.end()) return it->second;
    if (failedSkyboxPaths_.count(path)) return 0;

    std::vector<std::string> candidates{path};
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        candidates.push_back(path + ".m2");
    } else {
        std::string ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".mdx" || ext == ".mdl") candidates.push_back(path.substr(0, dot) + ".m2");
    }

    std::vector<uint8_t> modelData;
    std::string resolvedPath;
    for (const auto& candidate : candidates) {
        modelData = cachedAssetManager->readFileOptional(candidate);
        if (!modelData.empty()) {
            resolvedPath = candidate;
            break;
        }
    }
    if (modelData.empty()) {
        LOG_WARNING("Skybox model unavailable: ", path);
        failedSkyboxPaths_.insert(path);
        return 0;
    }

    pipeline::M2Model model = pipeline::M2Loader::load(modelData);
    model.name = resolvedPath + "#original-sky";
    const std::string skinPath = pipeline::skinPathForM2(resolvedPath);
    auto skinData = cachedAssetManager->readFileOptional(skinPath);
    if (!skinData.empty() && model.version >= 264) {
        pipeline::M2Loader::loadSkin(skinData, model);
    }
    if (!model.isValid()) {
        LOG_WARNING("Skybox model is invalid: ", resolvedPath);
        failedSkyboxPaths_.insert(path);
        return 0;
    }

    const uint32_t modelId = static_cast<uint32_t>(std::hash<std::string>{}(model.name));
    if (!skyboxModelRenderer_->loadModel(model, modelId)) {
        LOG_WARNING("Failed to upload skybox model: ", resolvedPath);
        failedSkyboxPaths_.insert(path);
        return 0;
    }
    loadedSkyModels_[path] = modelId;
    LOG_INFO("Skybox model loaded: ", resolvedPath);
    return modelId;
}

void Renderer::updateGlares(float deltaTime) {
    namespace sc = sky_celestial;
    float sunSky = 0.0f, sunSeen = 0.0f, moonSeen = 0.0f;
    if (lightingManager) {
        const float day = lightingManager->getTimeOfDayHours() / 24.0f;
        // What every glare shares (0x7ef6e0): a sky model over the sky, and
        // water over the camera.
        float shared = 1.0f;
        float skyModel = 0.0f;
        for (const auto& layer : lightingManager->getSkyboxLayers()) {
            skyModel = std::max(skyModel, layer.weight);
        }
        shared *= 1.0f - std::clamp(skyModel, 0.0f, 1.0f);
        if (const auto depth = cameraWaterDepth(); depth && *depth > 0.0f) {
            shared *= 1.0f - std::clamp(0.1f * *depth, 0.0f, 1.0f);
        }
        const Clouds* clouds = skySystem ? skySystem->getClouds() : nullptr;
        const auto coverageToward = [&](const glm::vec3& dir) {
            if (!clouds || !clouds->isEnabled()) return 0.0f;
            return cloud_coverage::coverageToward(dir, clouds->getDensity(), clouds->getWindOffset());
        };
        // Each body's own hours and the cloud in front of it; then how much of
        // its disc the world leaves in view, as the GPU last counted it.
        const auto seenOf = [&](SkyBodies::Body body) {
            return body == SkyBodies::Body::Sun ? publishedSun_ : publishedMoon_;
        };
        const sc::SkyDisc sun = sc::sunDisc(day);
        sunSky = shared * sc::lookup(sc::kSunGlareHours, day) *
                 sc::sunGlareThroughCloud(coverageToward(sun.direction));
        sunSeen = sunSky * seenOf(SkyBodies::Body::Sun);
        const sc::SkyDisc moon = sc::moonDisc(day);
        const float moonSky = shared * sc::lookup(sc::kMoonGlareHours, day) *
                              sc::moonGlareThroughCloud(coverageToward(moon.direction));
        moonSeen = moonSky * seenOf(SkyBodies::Body::Moon);
    }
    sunGlare_ = sc::easeGlare(sc::kSunGlare, sunGlare_, sunSeen, deltaTime);
    sunGlareSky_ = sc::easeGlare(sc::kSunGlare, sunGlareSky_, sunSky, deltaTime);
    moonGlare_ = sc::easeGlare(sc::kMoonGlare, moonGlare_, moonSeen, deltaTime);
}

void Renderer::drawGlares(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    namespace sc = sky_celestial;
    SkyBodies* bodies = skySystem ? skySystem->getBodies() : nullptr;
    if (!bodies || !frameCamera() || !frameInputs_.hasLighting) return;
    const float day = frameInputs_.timeOfDay / 24.0f;
    const glm::vec3 forward = glm::normalize(frameCamera()->getForward());
    const glm::vec3& colour = frameInputs_.lighting.sunColor;
    const float weather = sc::weatherFade(frameInputs_.weather.strength());
    // First ask, of the finished depth, how much of each disc the world
    // hides; the answer comes back a frame slot later (updateGlares).
    const uint32_t frame = vkCtx->getCurrentFrame();
    for (const SkyBodies::Body body : {SkyBodies::Body::Sun, SkyBodies::Body::Moon}) {
        bodies->queryDiscVisibility(cmd, perFrameSet, body,
                                    body == SkyBodies::Body::Sun ? sc::sunDisc(day) : sc::moonDisc(day),
                                    frame, frameCamera()->getViewMatrix(), frameCamera()->getProjectionMatrix(),
                                    activeRenderExtent_);
    }
    if (frameInputs_.sunGlare > 0.0f) {
        const sc::SkyDisc sun = sc::sunDisc(day);
        const sc::GlareShape shape = sc::sunGlareShape(glm::dot(forward, sun.direction));
        bodies->renderGlare(cmd, perFrameSet, SkyBodies::Body::Sun, sun.direction, shape.size,
                            colour, shape.alpha * frameInputs_.sunGlare * weather);
    }
    if (frameInputs_.moonGlare > 0.0f) {
        const sc::SkyDisc moon = sc::moonDisc(day);
        const sc::GlareShape shape =
            sc::moonGlareShape(glm::dot(forward, moon.direction), moon.scale);
        bodies->renderGlare(cmd, perFrameSet, SkyBodies::Body::Moon, moon.direction, shape.size,
                            colour, shape.alpha * frameInputs_.moonGlare * weather);
    }
}

void Renderer::logSunDiagnostic(const SkyParams& params) {
    if (!skySystem || !frameCamera()) return;
    const SkyBodies* bodies = skySystem->getBodies();
    SkyBodies::DiscDraw sun =
        bodies ? bodies->lastDiscDraw(SkyBodies::Body::Sun) : SkyBodies::DiscDraw{};
    SkyBodies::DiscDraw moon =
        bodies ? bodies->lastDiscDraw(SkyBodies::Body::Moon) : SkyBodies::DiscDraw{};
    if (skySystem->celestialSkipped()) {
        sun = moon = {.drawn = false, .skipped = "under a sky model"};
    }

    const glm::vec3 sunDir = params.sunDisc.direction;
    const SunOnScreen screen =
        sunScreenPosition(frameCamera()->getViewMatrix(), frameCamera()->getProjectionMatrix(), sunDir);
    const bool inView = screen.inFront && screen.uv.x >= 0.0f && screen.uv.x <= 1.0f &&
                        screen.uv.y >= 0.0f && screen.uv.y <= 1.0f;
    const float fromView = glm::degrees(std::acos(
        glm::clamp(glm::dot(glm::normalize(frameCamera()->getForward()), sunDir), -1.0f, 1.0f)));

    const SunDiagState state{
        .drawn = sun.drawn,
        .skipped = sun.skipped,
        .inView = inView,
        .angleStep = static_cast<int>(fromView / 5.0f),
        .occlusionStep = static_cast<int>(frameInputs_.sunGlare * 4.0f + 0.5f),
        .hourStep = static_cast<int>(params.timeOfDay * 4.0f),
        .weather = params.weatherIntensity > 0.01f,
        .moonDrawn = moon.drawn,
    };
    if (state == sunDiagLast_ || frameInputs_.time - sunDiagLoggedAt_ < 0.5) return;
    sunDiagLast_ = state;
    sunDiagLoggedAt_ = frameInputs_.time;

    const auto v3 = [](const glm::vec3& c) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "(%.2f %.2f %.2f)", c.r, c.g, c.b);
        return std::string(buf);
    };
    LOG_INFO("sun: ", sun.drawn ? "drawn" : "not drawn - ", sun.drawn ? "" : sun.skipped,
             " hour=", params.timeOfDay,
             " elev=", glm::degrees(std::asin(glm::clamp(sunDir.z, -1.0f, 1.0f))),
             " inView=", inView, " uv=(", screen.uv.x, ",", screen.uv.y, ")",
             " fromView=", fromView,
             " colour=", v3(params.sunColor), " alpha=", sun.alpha,
             " glare=", frameInputs_.sunGlare, " glareSky=", frameInputs_.sunGlareSky,
             " moon=", moon.drawn ? "drawn" : moon.skipped,
             " moonElev=", glm::degrees(std::asin(glm::clamp(params.moonDisc.direction.z, -1.0f, 1.0f))),
             " moonGlare=", frameInputs_.moonGlare, " clouds=", params.cloudDensity,
             " weather=", params.weatherIntensity,
             " fog=", volumetricThisFrame_ ? fogExtinction_ : 0.0f,
             " msaa=", int(vkCtx ? vkCtx->getMsaaSamples() : 1));
}

void Renderer::update(float deltaTime) {
    ZoneScopedN("Renderer::update");
    // The renderer's frame starts here when it is updated before it draws;
    // its parts, these included, are read after endFrame.
    if (vkCtx) {
        vkCtx->gameCpuStages().clear();
    }
    auto updateStart = std::chrono::steady_clock::now();
    globalTime += deltaTime;
    runDeferredWorldInitStep(deltaTime);
    if(distantTerrain_ && cachedAssetManager && terrainManager && terrainManager->isStreamingEnabled()) {
        const bool before=distantTerrain_->hasSource();
        distantTerrain_->setMap(cachedAssetManager,terrainManager->getMapName());
        distantTerrain_->update();
        if(before!=distantTerrain_->hasSource())setViewDistance(viewDistance_);
    }

    updateGlares(deltaTime);

    lastDeltaTime_ = deltaTime;

    // Where the update goes, every frame into the frame budget under
    // "rendererUpdate", and the whole breakdown in the log when one is slow.
    // The stage timer around this call said only that it took 50-70 ms a
    // frame for seconds at a time in Dalaran.
    struct UpdatePhase { const char* name; float ms; };
    std::array<UpdatePhase, 16> phases{};
    size_t phaseCount = 0;
    auto phaseStart = updateStart;
    auto endPhase = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        const float ms = std::chrono::duration<float, std::milli>(now - phaseStart).count();
        if (phaseCount < phases.size()) phases[phaseCount++] = {name, ms};
        if (vkCtx) vkCtx->gameCpuStages().add("rendererUpdate", name, ms);
        phaseStart = now;
    };

    // The last apply point of the frame: what Present asked since its start -
    // the player's own model posed after the camera - is made now. From here
    // to the end of the frame the instances are only read (scene_commands.hpp).
    applySceneCommands();
    endPhase("scene commands");

    collision_->resetQueryStats();

    // Resolve WMO containment before weather and ambience consume it. Server
    // weather remains authoritative outdoors, but particles must not follow the
    // camera through a roof into Ironforge or other enclosed WMOs.
    const glm::vec3 camPos = camera ? camera->getPosition() : glm::vec3(0.0f);
    uint32_t insideWmoId = 0;
    const bool insideWmo = camera &&
        collision_->insideWmo(collision::Reach::everywhere(), camPos.x, camPos.y, camPos.z,
                              &insideWmoId);
    playerIndoors_ = insideWmo;
    insideWmoId_ = insideWmoId;

    // Characters, creatures and their weapons take the light of the interior
    // group they stand in. WMO doodads are told theirs when they are placed.
    if (characterRenderer) {
        characterRenderer->updateInteriorLight(
            [this](const glm::vec3& p) -> std::optional<InteriorLight> {
                const auto sample = collision_->interiorAt(p.x, p.y, p.z);
                if (!sample) return std::nullopt;
                return unitInteriorLight(sample->ambient, sample->floorColor);
            },
            collision_->wmoRevision());
    }

    // The weather drawn this frame, coming toward the realm's or the dev
    // panel's. Before the light, which blends toward the storm row by it.
    weather_.setTransitionSeconds(weatherControls_.transitionSeconds);
    weather_.update(deltaTime, weatherControls_.weather.value_or(WeatherTarget{
                                   .kind = environment_.weatherKind,
                                   .intensity = environment_.weatherIntensity,
                                   .abrupt = environment_.weatherAbrupt}));

    // Update lighting system
    if (lightingManager) {
        // Indoors only inside one of a WMO's interior groups. Being inside any
        // group is not enough: Stormwind's streets are exterior groups of the
        // city's WMO, and so are porches, bridges and archways.
        lightingManager->setIndoors(insideWmo &&
            collision_->insideInteriorWmo(collision::Reach::everywhere(), camPos.x, camPos.y, camPos.z));

        // The camera's eye under the surface, not the player swimming: on the
        // surface the camera is above the water and sees the dry light.
        const auto eyeDepth = cameraWaterDepth();
        const bool isUnderwater = eyeDepth && *eyeDepth > 0.0f;
        const uint32_t resolvedZoneId = environment_.zoneId;

        endPhase("pre-lighting");
        lightingManager->update(deltaTime, characterPosition, environment_.mapId, resolvedZoneId,
                                environment_.gameTime, weatherStrength(), isUnderwater);
        hazeLightForWeather();
        endPhase("lighting");
    }

    updateWind(deltaTime);

    endPhase("weather and wind");
    // Update terrain streaming
    if (terrainManager && camera) {
        auto terrStart = std::chrono::steady_clock::now();
        terrainManager->update(*camera, deltaTime);
        float terrMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - terrStart).count();
        if (terrMs > 50.0f) {
            LOG_WARNING("SLOW terrainManager->update: ", terrMs, "ms");
        }
    }

    endPhase("terrain");
    // Update sky system (skybox time, star twinkle, clouds, celestial moon phases)
    if (skySystem) {
        if (auto* clouds = skySystem->getClouds())
            clouds->setDensity(weatherLight() ? weatherLight()->cloudDensity : 0.0f);
        skySystem->update(deltaTime);
    }
    if (updateSkyboxLayers() && skyboxModelRenderer_ && camera) {
        skyboxModelRenderer_->setSkyTimeOfDay(lightingManager
            ? lightingManager->getTimeOfDayHours()
            : (skybox ? skybox->getTimeOfDay() : 12.0f));
        skyboxModelRenderer_->update(deltaTime, camera->getViewMatrix(),
                                     camera->getProjectionMatrix());
    }

    // What falls, what is on the lens, and the lightning: after the wind,
    // which carries all of it.
    updateWeather(deltaTime);

    // Update swim effects
    if (swimEffects && camera) {
        swimEffects->update(*camera, viewerMotion_, characterPosition, liquidScene_, deltaTime);
    }

    // Whether there is water in view this frame: the sea, its wake, the sea
    // bed under it and its mirror are worked out only while there is.
    if (waterRenderer && camera) waterRenderer->updateInView(*camera);

    // What every body in the water leaves in it: the wake round the camera.
    updateWaterWake(deltaTime);

    // Surface disturbance the character leaves in the water. The droplet spray
    // is thrown by SwimEffects above; this is the froth on the surface itself,
    // which has to come from the water shader to move and light with the water.
    if (waterRenderer && camera) {
        const glm::vec3 charPos = viewerMotion_.thirdPerson ? characterPosition : camera->getPosition();
        const bool swimming = viewerMotion_.swimming;
        float intensity = 0.0f;
        bool wading = false;

        if (viewerMotion_.moving) {
            if (auto waterH = liquidScene_.heightAt(charPos.x, charPos.y)) {
                if (swimming) {
                    // A wake only exists where the swimmer meets the surface -
                    // diving deep leaves the surface undisturbed.
                    const float below = *waterH - charPos.z;
                    intensity = glm::clamp(1.0f - (below - 0.6f) / 1.4f, 0.0f, 1.0f);
                } else {
                    // Ankle-deep barely marks the water; thigh-deep throws the
                    // most, past which the character starts swimming anyway.
                    const float depth = *waterH - charPos.z;
                    if (depth > 0.03f && depth < 1.8f) {
                        wading = true;
                        intensity = glm::clamp(depth / 0.6f, 0.3f, 1.0f);
                    }
                }
            }
        }

        const float yawRad = glm::radians(viewerMotion_.yawDeg);
        const glm::vec2 travelDir(std::sin(yawRad), -std::cos(yawRad));
        waterRenderer->updateWake(deltaTime, glm::vec2(charPos.x, charPos.y),
                                  travelDir, intensity, wading);
    }

    // Update mount dust effects
    if (mountDust) {
        mountDust->update(deltaTime);

    }
    // Update level-up effect
    // Their expired instances go at the next apply point, with the scene's
    // other changes.
    if (levelUpEffect) {
        levelUpEffect->update(deltaTime, scene_->models());
    }
    // Update charge effect
    if (chargeEffect) {
        chargeEffect->update(deltaTime, scene_->models());
    }
    endPhase("sky/effects");


    // Launch M2 doodad animation on background thread (overlaps with character animation + audio)
    std::future<void> m2AnimFuture;
    bool m2AnimLaunched = false;
    if (m2Renderer && camera) {
        float m2DeltaTime = deltaTime;
        glm::mat4 m2View = camera->getViewMatrix();
        glm::mat4 m2Projection = camera->getProjectionMatrix();
        m2AnimFuture = core::ThreadPool::frameWorkers().submit("M2 animation",
            [this, m2DeltaTime, m2View, m2Projection]() {
                m2Renderer->update(m2DeltaTime, m2View, m2Projection);
            });
        m2AnimLaunched = true;
    }

    // Update character animations (runs in parallel with M2 animation above)
    if (characterRenderer && camera) {
        const glm::mat4 characterView = camera->getViewMatrix();
        characterRenderer->update(deltaTime, camera->getPosition(), &characterView);
    }
    endPhase("characters");


    // Joined before the footprints below, which query M2 floors. The update no
    // longer rebuilds the spatial index - every create, move and removal keeps
    // it current on this thread - but it once did, and joining after the
    // footsteps then let this thread walk the index while the worker cleared
    // it: a SIGSEGV in M2Renderer::gatherCandidates. Nothing the main thread
    // does to M2 instances may overlap the update until scene changes go
    // through a single point in the frame.
    if (m2AnimLaunched) {
        try { m2AnimFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("M2 animation worker: ", e.what()); }
        m2AnimLaunched = false;
    }
    endPhase("M2 animation wait");

    // Creatures' particles and ribbons, on the skeletons both updates above
    // have just finished - not while M2Renderer's worker is still running.
    if (characterEmitters_) characterEmitters_->update(deltaTime);
    endPhase("character emitters");

    // Footsteps: age visual prints, then let authored footfall events add new ones.
    if (footprintRenderer) footprintRenderer->update(deltaTime);
    endPhase("footprints");

    // Periodic cache hygiene: drop model GPU data no longer referenced by active instances.
    static float modelCleanupTimer = 0.0f;
    modelCleanupTimer += deltaTime;
    if (modelCleanupTimer >= 5.0f) {
        if (wmoRenderer) {
            wmoRenderer->cleanupUnusedModels();
        }
        endPhase("unused WMO models");
        if (m2Renderer) {
            m2Renderer->cleanupUnusedModels();
        }
        endPhase("unused M2 models");
        modelCleanupTimer = 0.0f;
    }
    auto updateEnd = std::chrono::steady_clock::now();
    lastUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
    if (lastUpdateMs > 40.0) {
        // At most once a second: a slow stretch is many frames in a row, and
        // one breakdown of it says as much as all of them.
        static std::chrono::steady_clock::time_point lastReport{};
        if (updateEnd - lastReport > std::chrono::seconds(1)) {
            lastReport = updateEnd;
            std::ostringstream line;
            line << "Renderer::update " << lastUpdateMs << "ms:";
            for (size_t i = 0; i < phaseCount; ++i) {
                if (phases[i].ms >= 1.0f) line << " " << phases[i].name << "=" << phases[i].ms;
            }
            LOG_WARNING(line.str());
        }
    }
}

void Renderer::runDeferredWorldInitStep(float deltaTime) {
    if (!deferredWorldInitEnabled_ || !deferredWorldInitPending_ || !cachedAssetManager) return;
    if (deferredWorldInitCooldown_ > 0.0f) {
        deferredWorldInitCooldown_ = std::max(0.0f, deferredWorldInitCooldown_ - deltaTime);
        if (deferredWorldInitCooldown_ > 0.0f) return;
    }

    if (deferredWorldInitStage_ == 0) drainFrames();
    switch (deferredWorldInitStage_) {
        case 0:
            if (questMarkerRenderer && !questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, cachedAssetManager))
                LOG_WARNING("Quest marker renderer re-init failed (non-fatal)");
            if (footprintRenderer && !footprintRenderer->initialize(this, vkCtx, perFrameSetLayout, cachedAssetManager))
                LOG_WARNING("Footprint renderer re-init failed (non-fatal)");
            break;
        default:
            deferredWorldInitPending_ = false;
            return;
    }

    deferredWorldInitStage_++;
    deferredWorldInitCooldown_ = 0.12f;
}

void Renderer::setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color) {
    if (overlaySystem_) overlaySystem_->setSelectionCircle(pos, radius, color);
}

void Renderer::clearSelectionCircle() {
    if (overlaySystem_) overlaySystem_->clearSelectionCircle();
}

// ========================= PostProcessPipeline delegation stubs (§4.3) =========================

PostProcessPipeline* Renderer::getPostProcessPipeline() const {
    return postProcessPipeline_.get();
}

void Renderer::setFSREnabled(bool enabled) {
    if (postProcessPipeline_) postProcessPipeline_->setFSREnabled(enabled);
}
void Renderer::setFSR2Enabled(bool enabled) {
    if (!postProcessPipeline_) return;
    // The FSR2 compute shaders need shaderStorageImageWriteWithoutFormat and
    // shaderInt16. A device without them used to be refused at startup; now it
    // starts, so the setting has to refuse instead.
    if (enabled) {
        VkContext* ctx = VkContext::globalInstance();
        if (ctx && !ctx->areFsr2ComputeFeaturesSupported()) {
            LOG_WARNING("FSR2 needs shaderStorageImageWriteWithoutFormat and shaderInt16, "
                        "which this device does not support - leaving it off");
            return;
        }
    }
    postProcessPipeline_->setFSR2Enabled(enabled);
}
void Renderer::setGrassEnabled(bool enabled) {
    if (enabled == grassEnabled_) return;
    grassEnabled_ = enabled;
    if (!enabled && grassRenderer_) {
        // Empty the population rather than just skipping the draw: a blade
        // count of zero is what stops the cull dispatching too, and the buffer
        // it was holding is the point of turning it off.
        grassRenderer_->setPopulation(nullptr, 0);
    }
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

void Renderer::setGrassScales(float density, float height) {
    // Up to triple. Density past 1 costs generation time and can meet the
    // blade cap, which thins the whole window rather than cutting a side off
    // it; height past 1 costs nothing.
    const float d = glm::clamp(density, 0.0f, 3.0f);
    const float h = glm::clamp(height, 0.5f, 3.0f);
    if (d == grassDensityScale_ && h == grassHeightScale_) return;
    grassDensityScale_ = d;
    grassHeightScale_ = h;
    // The live population was generated with the old numbers, so it has to go
    // rather than wait for the player to walk far enough to be rebuilt - and
    // so does a build in progress, which froze them when it began.
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

void Renderer::setGrassDistance(float yards) {
    // The slider's own range. The ceiling is generosity rather than promise:
    // a 0.4 yard blade is under a pixel tall past a couple of hundred yards,
    // so the far end of a long range is carried by the taller seeded stems
    // and by density, not by every blade surviving.
    const float d = glm::clamp(yards, 30.0f, 2000.0f);
    if (d == grassDistance_) return;
    grassDistance_ = d;
    if (grassRenderer_) grassRenderer_->setCullDistance(d);
    // Regenerate: the window radius and the falloff are both functions of the
    // distance, so neither the live population nor a build in progress
    // matches it any more.
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

void Renderer::updateWind(float deltaTime) {
    // The table, read once. Missing or malformed, it says so itself and a
    // built-in light wind blows instead.
    if (!windZonesLoaded_) {
        windZonesLoaded_ = true;
        windZones_ = pipeline::loadWindZonesFile("assets/wind_zones.json");
    }

    WindInputs inputs;
    if (environment_.hasRealmTime()) {
        inputs.clockSeconds = environment_.realmSeconds;
        if (!windRealmSynced_ && windUnsyncedSaid_) {
            LOG_INFO("Wind: the realm's clock has arrived; the wind is the realm's from now");
        }
        windRealmSynced_ = true;
    } else {
        // Not shared, but still a wind: the slots and phases come from this
        // machine's clock instead. Before the world is on screen there is no
        // realm to have a clock yet, which is not worth a line.
        windRealmSynced_ = false;
        inputs.clockSeconds = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (worldInView_ && !windUnsyncedSaid_) {
            windUnsyncedSaid_ = true;
            LOG_WARNING("Wind: the world is drawn without the realm's clock; the wind follows "
                        "this machine's clock, so other players see a different one");
        }
    }
    // The weather being drawn, the dev panel's when it has one; and its wind
    // over a scene's.
    inputs.weatherKind = weather_.target().kind;
    inputs.weatherIntensity = weather_.target().intensity;
    inputs.pivot = glm::dvec2(characterPosition.x, characterPosition.y);
    inputs.fixedWind = weatherControls_.wind ? weatherControls_.wind : environment_.wind;
    CpuStageTimes* stages = vkCtx ? &vkCtx->gameCpuStages() : nullptr;
    {
        std::optional<CpuStage> stage;
        if (stages) stage.emplace(*stages, "rendererUpdate/weather and wind", "wind field");
        wind_.update(deltaTime, inputs, windZones_.rowFor(environment_.areaId, environment_.zoneId),
                     windZones_);
    }
    if (waterRenderer) {
        std::optional<CpuStage> stage;
        if (stages) stage.emplace(*stages, "rendererUpdate/weather and wind", "water waves");
        const WindState& wind = wind_.state();
        waterRenderer->advanceWindWaves(deltaTime, wind.speed * wind.gust, wind.direction,
                                        glm::vec2(characterPosition.x, characterPosition.y));
    }
}

void Renderer::updateWaterWake(float deltaTime) {
    if (!waterRenderer || !camera) return;
    if (!waterRenderer->inView()) {
        waterRenderer->restWake(deltaTime);
        return;
    }
    const glm::vec3 eye = camera->getPosition();
    // The bodies whose middle is within the wake's reach of the camera, and
    // in water: the player, every NPC and creature the renderer draws.
    struct InReach {
        float distance;
        WaterMover mover;
    };
    std::vector<InReach> inReach;
    if (characterRenderer) {
        constexpr float kReach = 60.0f;  // the wake's field is 128 yards a side
        characterRenderer->forEachBody([&](const CharacterRenderer::BodyPlace& body) {
            const float distance = glm::length(glm::vec2(body.position) - glm::vec2(eye));
            if (distance > kReach) return;
            const auto surface = liquidScene_.nearestHeightAt(body.position.x, body.position.y, body.position.z);
            if (!surface) return;
            const float bottom = body.position.z + body.boundMin.z;
            const float top = body.position.z + body.boundMax.z;
            WaterMover m;
            m.id = body.instanceId;
            m.position = glm::vec2(body.position);
            m.facing = glm::vec2(std::cos(body.yaw), std::sin(body.yaw));
            const glm::vec2 extent = glm::vec2(body.boundMax - body.boundMin);
            m.halfLength = 0.5f * std::max(extent.x, extent.y);
            m.halfWidth = 0.5f * std::min(extent.x, extent.y);
            m.head = wakeHeadFor(bottom, top, *surface);
            if (m.head <= 0.0f) return;
            inReach.push_back({distance, m});
        });
    }
    // Nearest first: if there are more than the wake takes, the furthest go.
    std::sort(inReach.begin(), inReach.end(),
              [](const InReach& a, const InReach& b) { return a.distance < b.distance; });
    std::vector<WaterMover> movers;
    movers.reserve(inReach.size());
    for (const InReach& r : inReach) movers.push_back(r.mover);
    waterRenderer->advanceWake(deltaTime, glm::vec2(eye), movers);
}

uint32_t Renderer::grassProfileFor(uint32_t effectId, uint32_t areaId) {
    // The biome table, read once. Its absence is fine - grass then looks the
    // way the effect data alone says - but a file that exists and does not
    // parse is an authoring error worth a line.
    if (!grassBiomesLoaded_) {
        grassBiomesLoaded_ = true;
        std::ifstream f("assets/grass_biomes.json");
        if (f) {
            const std::string text((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            std::string parseError;
            grassBiomes_ = pipeline::loadGrassBiomes(text, parseError);
            if (!parseError.empty()) {
                LOG_WARNING("Grass biomes: ", parseError);
            } else {
                LOG_INFO("Grass biomes: ", grassBiomes_.size(), " regions");
            }
        }
    }

    // Which biome this ground belongs to, memoised per area: the zone walk
    // costs a few map lookups and this is called for every sampled blade.
    uint32_t biomeIdx = 0;
    const auto cachedBiome = grassBiomeForArea_.find(areaId);
    if (cachedBiome != grassBiomeForArea_.end()) {
        biomeIdx = cachedBiome->second;
    } else {
        const uint32_t zoneId = areaZoneResolver_ ? areaZoneResolver_(areaId) : areaId;
        biomeIdx = grassBiomes_.findFor(areaId, zoneId);
        grassBiomeForArea_[areaId] = biomeIdx;
    }

    // Derived once per (biome, effect) and kept. The table is uploaded whole
    // whenever it grows, which happens a handful of times as the player
    // crosses into ground they have not stood on before and then stops.
    const uint64_t key = (static_cast<uint64_t>(biomeIdx) << 32) | effectId;
    const auto known = grassProfileIndex_.find(key);
    if (known != grassProfileIndex_.end()) return known->second;

    std::vector<std::string> models;
    std::vector<uint32_t> weights;
    if (terrainManager) terrainManager->getGroundEffectDoodads(effectId, models, weights);

    pipeline::GrassProfile profile = pipeline::deriveProfile(models, weights);
    if (const auto* biome = grassBiomes_.biome(biomeIdx)) {
        biome->override_.apply(profile);
    }

    uint32_t index = 0;
    if (grassProfiles_.size() < GrassRenderer::kMaxProfiles) {
        index = static_cast<uint32_t>(grassProfiles_.size());
        grassProfiles_.push_back(profile);
    }
    grassProfileIndex_[key] = index;
    return index;
}

void Renderer::updateGrassPopulation() {
    if (!grassEnabled_) return;
    if (!grassRenderer_ || !grassRenderer_->isReady() || !terrainManager) return;

    // How far out blades are generated, and how far the player may walk before
    // the window is rebuilt. Both follow the grass distance setting. The
    // margin between them is the invariant: at its stalest the window centre
    // lags the player by a full step, so the grass ahead reaches
    // (radius - step) - and that must clear the cull distance, or the field
    // ends at a visible edge on one side of the player and pops forward on
    // every rebuild. The first numbers this had put the window *inside* the
    // cull distance, which is exactly how it looked; building the radius as
    // distance + step keeps the margin by construction at every distance the
    // slider allows.
    // Constant, not scaled by the distance setting: the octave walk made
    // rebuild cost grow with the log of the window rather than its area, so
    // frequent small steps beat rare big ones - and the slack every ring
    // must carry to cover a stale centre is the step, which at three
    // hundred yards would have meant full-density generation far past the
    // near field.
    const float rebuildStep = 24.0f;
    const float windowRadius = grassDistance_ + rebuildStep;
    const float rebuildStepSq = rebuildStep * rebuildStep;

    const glm::vec3 center = characterPosition;
    if (glm::dot(center, center) <= 0.0f) return;  // no character yet

    // Whether a new build has to start. While one is running the comparison
    // is against its centre rather than the live window's, so walking far
    // during a long build restarts it around where the player now is instead
    // of finishing a window they have already left.
    bool needBegin;
    if (grassBuilder_.active()) {
        const glm::vec2 moved(center.x - grassBuildCenter_.x, center.y - grassBuildCenter_.y);
        needBegin = glm::dot(moved, moved) >= rebuildStepSq;
    } else if (grassWindowValid_) {
        const glm::vec2 moved(center.x - grassWindowCenter_.x, center.y - grassWindowCenter_.y);
        needBegin = glm::dot(moved, moved) >= rebuildStepSq;
    } else {
        needBegin = true;
    }
    if (!needBegin && !grassBuilder_.active()) return;

    // One chunk decoded at a time. populateArea walks cells in world order, so
    // consecutive samples land in the same chunk and this memo almost always
    // hits - without it every candidate would decode the chunk's alpha maps
    // again, which is four kilobytes a layer for each of tens of thousands.
    const pipeline::MapChunk* cached = nullptr;
    const TerrainTile* cachedTile = nullptr;
    pipeline::ChunkGrassContext context;
    auto densityFor = [this](uint32_t effectId) {
        return terrainManager->getGroundEffectDensity(effectId);
    };
    // One-shot diagnostic: which link in the chain is empty. A population of
    // zero can mean no chunk under the sample, no effect id on its layers, or
    // an effect the table grows nothing for, and the three are indistinguish-
    // able from the outside.
    static bool reportedChain = false;
    size_t noChunk = 0;
    size_t sampled = 0;

    // A chunk is 33 yards across and candidates are a third of a yard apart, so
    // a hundred consecutive samples land in the chunk the last one did. Testing
    // that chunk before searching for another is what takes this off the frame:
    // findChunkAt is a tile lookup and a 3x3 probe, and running it per sample
    // was most of the cost.
    constexpr float kUnitSize = core::coords::TILE_SIZE / 16.0f / 8.0f;

    auto sampler = [&](float wx, float wy) -> pipeline::GrassSuitability {
        float fracX = 0.0f;
        float fracY = 0.0f;
        ++sampled;
        const pipeline::MapChunk* chunk = nullptr;
        if (cached && pipeline::TerrainMeshGenerator::chunkFractionsAt(
                          cached->position, wx, wy, kUnitSize, fracX, fracY)) {
            chunk = cached;
        } else {
            chunk = terrainManager->findChunkAt(wx, wy, fracX, fracY, &cachedTile);
        }
        if (!chunk) { ++noChunk; return {}; }
        if (chunk != cached) {
            // The texture names live on the tile, which pipeline code never
            // sees, so they arrive as a lookup. build() uses them to keep
            // grass off made surfaces; nothing else can, because a road's
            // ground effect is as real as a meadow's.
            const TerrainTile* tile = cachedTile;
            auto textureNameFor = [tile](uint32_t texId) -> std::string {
                if (!tile || texId >= tile->terrain.textures.size()) return {};
                return tile->terrain.textures[texId];
            };
            context = pipeline::ChunkGrassContext{};
            context.build(*chunk, densityFor, textureNameFor);

            // Reported for the first few chunks that carry a made surface, at
            // a level the default log actually keeps. Grass on cobblestone has
            // survived two fixes that each looked right, so this is the code
            // saying what it sees rather than me saying what it should - and
            // aimed at the chunks in question rather than at whichever chunk
            // happened to be sampled first.
            static int reportedRoadChunks = 0;
            bool anyRoad = false;
            for (size_t i = 0; i < std::min<size_t>(chunk->layers.size(), 4); ++i) {
                anyRoad = anyRoad ||
                          pipeline::isRoadLikeTexture(textureNameFor(chunk->layers[i].textureId));
            }
            if (anyRoad && reportedRoadChunks < 4) {
                ++reportedRoadChunks;
                std::string detail;
                for (size_t i = 0; i < std::min<size_t>(chunk->layers.size(), 4); ++i) {
                    const std::string name = textureNameFor(chunk->layers[i].textureId);
                    detail += "\n    [" + std::to_string(i) + "] tex=" +
                              std::to_string(chunk->layers[i].textureId) + " '" +
                              (name.empty() ? std::string("<no name>") : name) + "' effect=" +
                              std::to_string(chunk->layers[i].effectId) +
                              (pipeline::isRoadLikeTexture(name) ? " ROAD" : "") +
                              (context.grows[i] ? " grows" : " bare");
                }
                LOG_WARNING("Grass road chunk: ", chunk->layers.size(), " layers", detail);
            }

            // Any water over this chunk. Sampled once here rather than per
            // blade: a pond's surface is flat, and three hundred thousand
            // height queries a rebuild is not.
            {
                const float cx = chunk->position[0] - 4.0f * kUnitSize;
                const float cy = chunk->position[1] - 4.0f * kUnitSize;
                if (const auto level = liquidScene_.heightAt(cx, cy)) {
                    context.waterHeight = *level;
                    context.hasWater = true;
                }
            }

            // The tones the ground is painted in, for colouring the blades.
            const size_t n = std::min<size_t>(chunk->layers.size(), 4);
            for (size_t i = 0; i < n; ++i) {
                const std::string name = textureNameFor(chunk->layers[i].textureId);
                if (name.empty()) continue;
                const auto tones = terrainManager->getTerrainTextureTones(name);
                context.layerShadow[i] = tones.shadow;
                context.layerHighlight[i] = tones.highlight;
                context.hasLayerColors = true;
            }
            cached = chunk;
            if (!reportedChain) {
                reportedChain = true;
                std::string layers;
                for (size_t i = 0; i < chunk->layers.size(); ++i) {
                    layers += " [" + std::to_string(i) +
                              "] effect=" + std::to_string(chunk->layers[i].effectId) +
                              " density=" +
                              std::to_string(terrainManager->getGroundEffectDensity(
                                  chunk->layers[i].effectId));
                }
                const auto fit = pipeline::evaluateGrass(context, *chunk, fracX, fracY);
                LOG_INFO("Grass chain: chunk at (", wx, ",", wy, ") frac=(", fracX, ",", fracY,
                         ") layers=", chunk->layers.size(), layers,
                         " -> suitability=", fit.suitability, " slope=", fit.slope);
            }
        }
        auto fit = pipeline::evaluateGrass(context, *chunk, fracX, fracY);
        // The terrain's verge eased toward roads; the placed world eases the
        // same number toward walls and wagon wheels.
        if (fit.suitability > 0.0f) {
            fit.wildness = std::min(fit.wildness, grassClearing_.wildness(wx, wy));
        }
        return fit;
    };

    pipeline::GrassPopulationParams params;
    params.densityScale = grassDensityScale_;
    params.baseHeight *= grassHeightScale_;
    // Full density out to the base range; past it the lattice coarsens by
    // octaves, so the long ranges the slider allows cost blades - and build
    // time - by the log of the radius rather than by its area. The slack
    // keeps blades in the buffer a rebuild step before they fade in, so a
    // ride toward them never outruns the window.
    params.fullDensityRadius = GrassRenderer::kCullDistance;
    params.ringSlack = rebuildStep;

    // Zero means the player turned grass off. Said out loud once: a density of
    // zero produces an empty population through a chain that is otherwise
    // working perfectly, and nothing about that looks like a setting rather
    // than a fault.
    if (params.densityScale <= 0.0f) {
        static bool warnedDisabled = false;
        if (!warnedDisabled) {
            warnedDisabled = true;
            LOG_WARNING("Grass: density is 0, so no grass will grow. "
                        "Raise Grass Density in Settings > Graphics to see any.");
        }
        grassBuilder_ = pipeline::GrassPopulationBuilder{};
        grassRenderer_->setPopulation(nullptr, 0);
        grassWindowCenter_ = center;
        grassWindowValid_ = true;
        return;
    }

    if (needBegin) {
        // Clearings around the built and the placed, gathered once per
        // rebuild: the terrain's own verges handle roads and paths, but a
        // building is a placement, and only its renderer knows where it is.
        std::vector<pipeline::GrassClearingSource> clearings;
        const float winMinX = center.x - windowRadius;
        const float winMaxX = center.x + windowRadius;
        const float winMinY = center.y - windowRadius;
        const float winMaxY = center.y + windowRadius;
        if (wmoRenderer) {
            wmoRenderer->collectGrassClearings(winMinX, winMinY, winMaxX, winMaxY, clearings);
        }
        if (m2Renderer) {
            m2Renderer->collectGrassClearings(winMinX, winMinY, winMaxX, winMaxY, clearings);
        }
        grassClearing_.build(std::move(clearings), winMinX, winMinY, winMaxX, winMaxY);

        grassBuilder_.begin(center.x, center.y, windowRadius, params,
                            GrassRenderer::kMaxBlades);
        grassBuildCenter_ = center;
    }

    // WOWEE_GRASS_DEBUG=1: everything about the ground the player is standing
    // on. Screenshots cannot say whether a stretch of cobble resolved to a
    // road layer that was suppressed, a dirt layer that was not, or a grass
    // layer underneath showing through - and those want three different fixes.
    // Once per build, not per slice.
    if (needBegin && rendering::envFlagEnabled("WOWEE_GRASS_DEBUG")) {
        float fx = 0.0f;
        float fy = 0.0f;
        const TerrainTile* tile = nullptr;
        if (const pipeline::MapChunk* here =
                terrainManager->findChunkAt(center.x, center.y, fx, fy, &tile)) {
            auto nameFor = [tile](uint32_t texId) -> std::string {
                if (!tile || texId >= tile->terrain.textures.size()) return {};
                return tile->terrain.textures[texId];
            };
            pipeline::ChunkGrassContext ctx;
            ctx.build(*here, densityFor, nameFor);
            std::string report;
            for (size_t i = 0; i < std::min<size_t>(here->layers.size(), 4); ++i) {
                const uint32_t texId = here->layers[i].textureId;
                const std::string name = nameFor(texId).empty() ? std::string("<none>")
                                                                : nameFor(texId);
                const bool road = pipeline::isRoadLikeTexture(name);
                report += "\n    [" + std::to_string(i) + "] effect=" +
                          std::to_string(here->layers[i].effectId) + " density=" +
                          std::to_string(terrainManager->getGroundEffectDensity(
                              here->layers[i].effectId)) +
                          (road ? " ROAD-SUPPRESSED " : " ") + name;
            }
            const auto fit = pipeline::evaluateGrass(ctx, *here, fx, fy);
            // The no-effect mask in full, plus the player's own quad. Stand on
            // a farm row or an abbey floor and this is the line that says
            // whether the data flags it and whether the bit order reads it
            // the right way round.
            const int qx = std::clamp(static_cast<int>(fx), 0, 7);
            const int qy = std::clamp(static_cast<int>(fy), 0, 7);
            LOG_INFO("Grass under player: frac=(", fx, ",", fy, ") suitability=",
                     fit.suitability, " growsAnything=", ctx.growsAnything ? 1 : 0,
                     " noEffectDoodad=0x", std::hex, here->noEffectDoodad, std::dec,
                     " thisQuad=", here->isEffectDisabled(qy, qx) ? "no-grow" : "grows",
                     " mappedLayer=", here->effectLayerFor(qy, qx),
                     report);
        }
    }

    const size_t profilesBefore = grassProfiles_.size();
    auto profileFor = [this](uint32_t effectId, uint32_t areaId) {
        pipeline::GrassProfileRef ref;
        ref.index = grassProfileFor(effectId, areaId);
        const auto& p = grassProfiles_[ref.index];
        ref.heightScale = p.heightScale;
        ref.widthScale = p.widthScale;
        ref.densityScale = p.densityScale;
        return ref;
    };

    // A bounded number of lattice blocks per frame, so a rebuild costs the
    // same per frame at any window size - a large one just takes more frames
    // and the field grows in when it lands. Since the octave walk, nearly
    // every block surveyed becomes a terrain sample, so the budget is set by
    // the cost of sampling rather than of walking: small enough to stay off
    // the frame, large enough that a default window lands in a few slices.
    constexpr size_t kCellsPerSlice = 64000;
    const auto started = std::chrono::steady_clock::now();
    const bool done = grassBuilder_.step(sampler, profileFor, kCellsPerSlice);

    // Only when it grew, and after every slice rather than at the end: the
    // shaders index this by blade, so it has to reach the device before the
    // population that refers to it does.
    if (grassProfiles_.size() != profilesBefore) {
        grassRenderer_->setProfiles(grassProfiles_);
    }
    const double generateMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    grassWorstGenerateMs_ = std::max(grassWorstGenerateMs_, generateMs);
    if (!done) return;

    std::vector<pipeline::GrassBladeSample>& blades = grassBuilder_.blades();
    const bool complete = grassBuilder_.complete();
    if (!complete) {
        // The window met the blade cap. The generator thins uniformly when it
        // can see this coming, so meeting it anyway means the density slider
        // and the terrain conspired past the estimate - worth a line, because
        // the visible symptom is a field that ends early on its north side.
        LOG_WARNING("Grass population truncated at ", blades.size(),
                    " blades; lower Grass Density or Grass Distance");
    }

    // WOWEE_GRASS_DUMP=1: write the first generated population to a CSV so
    // the field's actual shape around the player can be analysed offline. The
    // cull reports a stable 2-3x more grass kept looking one way than the
    // other, which is a claim about where blades are, and this answers it.
    static const bool dumpPopulation = rendering::envFlagEnabled("WOWEE_GRASS_DUMP");
    if (dumpPopulation) {
        static bool dumped = false;
        if (!dumped && !blades.empty()) {
            dumped = true;
            std::ofstream out("grass_population.csv");
            out << "# player," << center.x << "," << center.y << "," << center.z << "\n";
            for (const auto& b : blades) {
                out << b.x << "," << b.y << "," << b.z << "," << b.height << "\n";
            }
            LOG_INFO("Grass population dumped: ", blades.size(), " blades");
        }
    }

    grassRenderer_->setPopulation(blades.data(), blades.size());
    // The build's centre, not where the player stands now: they may have
    // walked most of a rebuild step while the window generated, and measuring
    // future movement from here rather than from them is what keeps the
    // stale-window margin honest.
    grassWindowCenter_ = grassBuildCenter_;
    grassWindowValid_ = true;
    ++grassRebuilds_;
    grassLastCount_ = blades.size();

    const double totalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (!blades.empty()) {
        float minZ = blades[0].z, maxZ = blades[0].z;
        float minH = blades[0].height, maxH = blades[0].height;
        for (const auto& b : blades) {
            minZ = std::min(minZ, b.z); maxZ = std::max(maxZ, b.z);
            minH = std::min(minH, b.height); maxH = std::max(maxH, b.height);
        }
        LOG_INFO("Grass extent: player=(", center.x, ",", center.y, ",", center.z,
                 ") bladeZ=[", minZ, ",", maxZ, "] height=[", minH, ",", maxH,
                 "] first=(", blades[0].x, ",", blades[0].y, ",", blades[0].z, ")");
    }
    if (blades.empty()) {
        LOG_INFO("Grass population empty: ", sampled, " samples, ", noChunk,
                 " with no chunk under them");
    }
    LOG_INFO("Grass population rebuilt: ", blades.size(), " blades, final slice ",
             static_cast<int>(totalMs), "ms (generate ", static_cast<int>(generateMs),
             "ms, density ", params.densityScale, ", distance ", grassDistance_, ")",
             complete ? "" : " - hit the blade cap");
}

void Renderer::renderSkyBackground(float timeOfDay,
                                   bool drawSkyModels, bool useOriginalSkybox) {
    const uint32_t slot = vkCtx->getCurrentFrame();
    if (!skyBackground_ || !skyBackground_->view(slot)) return;

    currentFrameData.fogSkyMiddle.w = 1.0f;
    std::memcpy(perFrameUBOMapped[slot], &currentFrameData, sizeof(currentFrameData));

    // Render only the background, including authored sky layers and the air.
    skyBackground_->begin(currentCmd, slot);
    auto params = skyParamsFromLighting(
        timeOfDay, frameInputs_.environment.gameTime, frameInputs_.weather.strength(),
        frameInputs_.hasLighting ? (&frameInputs_.weatherLight) : nullptr,
        useOriginalSkybox);
    params.sunGlareVisibility = 0.0f; // Lens flare is an optical overlay, not background haze.
    params.drawBodies = false;        // Nor are the discs: the haze is the sky behind them.
    if (drawSkyModels) skyboxModelRenderer_->prepareRender(slot, *frameCamera());
    skySystem->record(currentCmd, skyPerFrameDescSets[slot], *frameCamera(), params,
                      frameInputs_.scene->sky, drawSkyModels ? skyboxModelRenderer_.get() : nullptr);
    skyBackground_->end(currentCmd, slot);
}

void Renderer::renderWorld() {
    ZoneScopedN("Renderer::renderWorld");

    // Guard against null command buffer (e.g. after VK_ERROR_DEVICE_LOST)
    if (currentCmd == VK_NULL_HANDLE) return;

    // GPU crash diagnostic: skip ALL world rendering to isolate crash source
    static const bool skipAll = (std::getenv("WOWEE_SKIP_ALL_RENDER") != nullptr);
    if (skipAll) return;

    worldDrawnLastFrame_ = true;
    worldDrawnThisFrame_ = true;

    auto renderStart = std::chrono::steady_clock::now();
    lastTerrainRenderMs = 0.0;
    lastWMORenderMs = 0.0;
    lastM2RenderMs = 0.0;

    uint32_t frameIdx = vkCtx->getCurrentFrame();
    VkDescriptorSet perFrameSet = perFrameDescSets[frameIdx];
    const glm::mat4& view = frameCamera() ? frameCamera()->getViewMatrix() : glm::mat4(1.0f);
    const glm::mat4& projection = frameCamera() ? frameCamera()->getProjectionMatrix() : glm::mat4(1.0f);

    // GPU crash diagnostic: skip individual renderers to isolate which one faults
    static const bool envSkipWMO = (std::getenv("WOWEE_SKIP_WMO") != nullptr);
    static const bool envSkipChars = (std::getenv("WOWEE_SKIP_CHARS") != nullptr);
    static const bool envSkipM2 = (std::getenv("WOWEE_SKIP_M2") != nullptr);
    static const bool envSkipTerrain = (std::getenv("WOWEE_SKIP_TERRAIN") != nullptr);
    static const bool envSkipSky = (std::getenv("WOWEE_SKIP_SKY") != nullptr);
    // ...and the ablation switches the same passes off, one phase at a time.
    const auto ablated = [&](AblationPass p) {
        return passAblation_ && passAblation_->skip(p);
    };
    const bool skipWMO = envSkipWMO || ablated(AblationPass::WMO);
    const bool skipChars = envSkipChars || ablated(AblationPass::Characters);
    const bool skipM2 = envSkipM2 || ablated(AblationPass::M2);
    const bool skipTerrain = envSkipTerrain || ablated(AblationPass::Terrain);
    const bool skipSky = envSkipSky || ablated(AblationPass::Sky);
    const bool skipGrass = ablated(AblationPass::Grass);
    // The terrain chooses its chunks' detail by their triangles' size in the
    // image it is drawn into, which an upscaler makes smaller than the window.
    if (m2Renderer) {
        m2Renderer->setSkipGroundDetail(ablated(AblationPass::Clutter));
        // 250 yards: past the shadow distance and well past where a tree with
        // no LOD is still worth every triangle it has.
        m2Renderer->setDoodadDistanceCap(ablated(AblationPass::FarDoodads) ? 250.0f : 0.0f);
    }

    // Get time of day for sky-related rendering
    const float timeOfDay = frameInputs_.timeOfDay;
    // Two questions, apart while the sky crossfades. The sky models are drawn
    // whenever any is up at all. The client's own stars, sun, moons and clouds
    // go only under a model that is fully up and does not ask for them, as in
    // WoW.exe: while a sky fades in, while two crossfade at a border, and
    // under the auroras they are drawn and the models go over them. This was
    // a hand-over at half the summed weight, which took them away under every
    // sky model, auroras included, and at every border.
    const bool drawSkyModels = frameInputs_.drawSkyModels;
    const bool useOriginalSkybox = frameInputs_.hideProceduralSky;


    // ── Multithreaded secondary command buffer recording ──
    // Terrain, WMO, and M2 record on worker threads while main thread handles
    // sky, characters, water, and effects.  prepareRender() on main thread first
    // to handle thread-unsafe GPU allocations (descriptor pools, bone SSBOs).
    if (parallelRecordingEnabled_) {
        // --- Pre-compute state + GPU allocations on main thread (not thread-safe) ---
        auto prepStart = std::chrono::steady_clock::now();
        if (wmoRenderer) wmoRenderer->prepareRender();
        auto prepWmoEnd = std::chrono::steady_clock::now();
        if (m2Renderer && frameCamera()) m2Renderer->prepareRender(frameIdx, *frameCamera());
        if (drawSkyModels && frameCamera())
            skyboxModelRenderer_->prepareRender(frameIdx, *frameCamera());
        // The characters' bones went up in beginFrame, before the shadow maps.
        auto prepM2End = std::chrono::steady_clock::now();
        const double prepWmoMs  = std::chrono::duration<double, std::milli>(prepWmoEnd - prepStart).count();
        const double prepM2Ms   = std::chrono::duration<double, std::milli>(prepM2End - prepWmoEnd).count();
        CpuStageTimes& stages = vkCtx->cpuStages();
        stages.add("renderWorld", "prepare WMO", static_cast<float>(prepWmoMs));
        stages.add("renderWorld", "prepare M2", static_cast<float>(prepM2Ms));
        // From here to the wait the main thread hands work out and records its
        // own share; then it waits for the slowest worker.
        std::optional<CpuStage> dispatchStage(std::in_place, stages, "renderWorld", "dispatch");

        // --- Dispatch worker threads (terrain + WMO + M2) ---
        std::future<double> terrainFuture, wmoFuture, charFuture, m2Future, postFuture;

        // Grass rides in the terrain secondary: it sits on the ground, and
        // that buffer is executed after the sky and before WMO, which is
        // exactly the order grass wants. Recording it there touches only the
        // one worker's command buffer.
        //
        // So the buffer is recorded when EITHER wants it, not when terrain
        // does. Hanging it off terrain alone meant that switching terrain off
        // took grass with it - which for a crash bisect is harmless and for an
        // ablation is the whole measurement, since the terrain phase would
        // then be reporting what terrain and grass cost together.
        const bool drawTerrain = terrainRenderer && frameCamera() && frameInputs_.terrainEnabled && !skipTerrain;
        const bool drawGrass = grassRenderer_ && !skipGrass;
        if (drawTerrain || drawGrass) {
            terrainFuture = core::ThreadPool::frameWorkers().submit("record: terrain", [&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_TERRAIN);
                setSecondaryViewportScissor(cmd);
                if (drawTerrain) {
                    if(distantTerrain_)distantTerrain_->record(cmd,perFrameSet,frameInputs_.scene->distantTerrain);
                    terrainRenderer->recordView(cmd, perFrameSet, frameInputs_.scene->terrain);
                }
                if (drawGrass) grassRenderer_->record(cmd, frameIdx, perFrameSet, frameInputs_.scene->grass);
                vkEndCommandBuffer(cmd);
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        if (wmoRenderer && frameCamera() && !skipWMO) {
            wmoFuture = core::ThreadPool::frameWorkers().submit("record: WMO", [&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_WMO);
                setSecondaryViewportScissor(cmd);
                wmoRenderer->render(cmd, perFrameSet, *frameCamera(), &frameInputs_.characterPosition);
                vkEndCommandBuffer(cmd);
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        if (m2Renderer && frameCamera() && !skipM2) {
            m2Future = core::ThreadPool::frameWorkers().submit("record: M2", [&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_M2);
                setSecondaryViewportScissor(cmd);
                const auto tBegin = std::chrono::steady_clock::now();
                m2Renderer->render(cmd, perFrameSet, *frameCamera());
                const auto tModels = std::chrono::steady_clock::now();
                // Blended things go after the water when it has a pass of its
                // own; see M2Renderer::renderAfterWater.
                const bool blendedHere = !m2Renderer->drawsBlendedAfterWater();
                if (blendedHere) m2Renderer->renderM2Particles(cmd, perFrameSet);
                const auto tParts = std::chrono::steady_clock::now();
                if (blendedHere) m2Renderer->renderM2Ribbons(cmd, perFrameSet);
                const auto tParticles = std::chrono::steady_clock::now();
                vkEndCommandBuffer(cmd);

                // This worker is the critical path of renderWorld, and the
                // model pass inside it accounts for barely a fifth of what it
                // takes: the cull, the sort and the draw recording together
                // measure half a millisecond against the three this returns.
                // So the rest is timed too - the particles and ribbons that
                // share the buffer, and ending the buffer itself, which on
                // this driver is where a secondary is actually encoded.
                static const bool prof = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
                if (prof) {
                    static auto lastSaid = std::chrono::steady_clock::now();
                    const auto tEnd = std::chrono::steady_clock::now();
                    if (tEnd - lastSaid > std::chrono::seconds(10)) {
                        lastSaid = tEnd;
                        const auto ms = [](auto a, auto b) {
                            return std::chrono::duration<double, std::milli>(b - a).count();
                        };
                        LOG_WARNING("  m2 worker: begin ", ms(t0, tBegin),
                                    "ms, models ", ms(tBegin, tModels),
                                    "ms, particles ", ms(tModels, tParts),
                                    "ms, ribbons ", ms(tParts, tParticles),
                                    "ms, endCommandBuffer ", ms(tParticles, tEnd), "ms");
                    }
                }
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        // --- Main thread: record sky (SEC_SKY) ---
        {
            VkCommandBuffer cmd = beginSecondary(SEC_SKY);
            setSecondaryViewportScissor(cmd);
            if (skySystem && frameCamera() && !skipSky) {
                rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                    timeOfDay,
                    frameInputs_.environment.gameTime, frameInputs_.weather.strength(),
                    frameInputs_.hasLighting ? (&frameInputs_.weatherLight) : nullptr,
                    useOriginalSkybox);
                // As much as the glare shows, weather included, as the client
                // puts its glare out (0x7f37c3).
                skyParams.sunGlareVisibility =
                    frameInputs_.sunGlare * sky_celestial::weatherFade(frameInputs_.weather.strength());
                skySystem->record(cmd, perFrameSet, *frameCamera(), skyParams, frameInputs_.scene->sky,
                                  drawSkyModels ? skyboxModelRenderer_.get() : nullptr);
                logSunDiagnostic(skyParams);
            }
            vkEndCommandBuffer(cmd);
        }

        // --- Main thread: record selection circle before overlay state is used by post ---
        {
            VkCommandBuffer cmd = beginSecondary(SEC_SELECTION);
            setSecondaryViewportScissor(cmd);
            if (overlaySystem_) {
                overlaySystem_->recordSelectionCircle(view, projection, cmd, frameInputs_.selection);
            }
            vkEndCommandBuffer(cmd);
        }

        // Character recording is independent after prepareRender() and no
        // longer shares the selection-circle overlay command buffer.
        // The faded ones go in a buffer of their own, queued after the M2s, or
        // after the water in its pass: drawn here they would hide the effects
        // inside them. Recorded by the same worker, one after the other.
        charFuture = core::ThreadPool::frameWorkers().submit("record: characters", [&]() -> double {
            auto t0 = std::chrono::steady_clock::now();
            VkCommandBuffer cmd = beginSecondary(SEC_CHARS);
            setSecondaryViewportScissor(cmd);
            if (characterRenderer && frameCamera() && !skipChars) {
                characterRenderer->render(cmd, perFrameSet, *frameCamera(), CharacterRenderer::Part::Solid);
            }
            vkEndCommandBuffer(cmd);
            if (!fadedCharactersAfterWater()) {
                VkCommandBuffer faded = beginSecondary(SEC_CHARS_FADED);
                setSecondaryViewportScissor(faded);
                if (characterRenderer && frameCamera() && !skipChars) {
                    characterRenderer->render(faded, perFrameSet, *frameCamera(), CharacterRenderer::Part::Faded);
                }
                vkEndCommandBuffer(faded);
            }
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        });

        // Post-world systems are disjoint from terrain/WMO/M2/characters. Start
        // this after selection recording so OverlaySystem is never used from
        // two threads at once.
        postFuture = core::ThreadPool::frameWorkers().submit("record: post", [&]() -> double {
            auto t0 = std::chrono::steady_clock::now();
            VkCommandBuffer cmd = beginSecondary(SEC_POST);
            setSecondaryViewportScissor(cmd);
            if (waterRenderer && frameCamera() && !waterDrawsInContinuePass() && !waterAblated()) {
                waterRenderer->setRenderExtent(activeRenderExtent_);
                waterRenderer->recordView(cmd, perFrameSet, *frameCamera(), frameInputs_.scene->water, false, frameIdx);
            }
            if (precipitation_ && frameCamera() && !precipitationDrawsWithWater_) {
                precipitation_->record(cmd, perFrameSet, frameIdx, frameInputs_.scene->precipitation);
            }
            if (lightning && frameCamera()) lightning->record(cmd, perFrameSet, frameInputs_.scene->lightning);
            if (swimEffects && frameCamera() && !swimEffectsDrawWithWater_) {
                swimEffects->record(cmd, perFrameSet, frameInputs_.scene->swim);
            }
            if (mountDust && frameCamera()) mountDust->record(cmd, perFrameSet, frameInputs_.scene->dust);
            if (chargeEffect && frameCamera()) chargeEffect->record(cmd, perFrameSet, frameInputs_.scene->charge);
            if (footprintRenderer && frameCamera()) footprintRenderer->record(cmd, perFrameSet, frameInputs_.scene->footprints);
            if (questMarkerRenderer && frameCamera()) questMarkerRenderer->record(cmd, perFrameSet, frameInputs_.scene->markers);
            drawGlares(cmd, perFrameSet);

            renderUnderwaterOverlay(cmd);
            renderPostSceneOverlays(cmd);
            vkEndCommandBuffer(cmd);
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        });

        // --- Wait for workers ---
        // Guard with try-catch: future::get() re-throws any exception from the
        // async task. Without this, a single bad_alloc in a render worker would
        // propagate as an unhandled exception and terminate the process.
        dispatchStage.reset();
        std::optional<CpuStage> waitStage(std::in_place, stages, "renderWorld", "wait for workers");
        try { if (terrainFuture.valid()) lastTerrainRenderMs = terrainFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Terrain render worker: ", e.what()); }
        try { if (wmoFuture.valid()) lastWMORenderMs = wmoFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("WMO render worker: ", e.what()); }
        try { if (m2Future.valid()) lastM2RenderMs = m2Future.get(); }
        catch (const std::exception& e) { LOG_ERROR("M2 render worker: ", e.what()); }
        try { if (charFuture.valid()) (void)charFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Character render worker: ", e.what()); }
        try { if (postFuture.valid()) (void)postFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Post render worker: ", e.what()); }
        waitStage.reset();

        // prepareRender() does the GPU allocations that are not thread-safe, so it runs
        // on the main thread and is not covered by the worker timings. Name the culprit
        // when a frame runs long instead of leaving renderWorld as one opaque number.
        const double prepTotalMs = prepWmoMs + prepM2Ms;
        const double worstWorkerMs = std::max({lastTerrainRenderMs, lastWMORenderMs, lastM2RenderMs});

        // The same breakdown on a timer, not only when a frame runs long.
        //
        // renderWorld is 3.3ms of a 15.8ms frame and the CPU and the GPU are
        // within a millisecond of each other now, so what the main thread
        // spends here decides the frame as much as any pass does - and the
        // 40ms threshold below only ever speaks when something has already
        // gone wrong. This says where the steady state goes.
        static const bool frameProfile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
        if (frameProfile) {
            static auto lastSaid = std::chrono::steady_clock::now();
            const auto sayNow = std::chrono::steady_clock::now();
            if (sayNow - lastSaid > std::chrono::seconds(10)) {
                lastSaid = sayNow;
                LOG_WARNING("  renderWorld: prepare ", prepTotalMs,
                            "ms (wmo ", prepWmoMs, " m2 ", prepM2Ms, "), workers terrain ",
                            lastTerrainRenderMs, " wmo ", lastWMORenderMs,
                            " m2 ", lastM2RenderMs, " (worst ", worstWorkerMs,
                            "), terrain chunks drawn ",
                            terrainRenderer ? terrainRenderer->getRenderedChunkCount() : 0,
                            " culled ",
                            terrainRenderer ? terrainRenderer->getCulledChunkCount() : 0);
                if (terrainRenderer) {
                    // Whether the resident tiles' geometry fits the shared
                    // pages, or chunks are drawn from buffers of their own.
                    const TerrainGeometryStats geo = terrainRenderer->geometryStats();
                    LOG_WARNING("  terrain geometry: ", terrainRenderer->getChunkCount(),
                                " chunks resident, ", geo.pages, " pages, vertices ",
                                geo.verticesUsed, "/", geo.vertexCapacity, ", indices ",
                                geo.indicesUsed, "/", geo.indexCapacity, ", ",
                                geo.ownBufferChunks, " chunks with buffers of their own");
                }
            }
        }

        if (prepTotalMs + worstWorkerMs > 40.0) {
            LOG_WARNING("SLOW renderWorld breakdown: prepare=", prepTotalMs,
                        "ms (wmo=", prepWmoMs, " m2=", prepM2Ms,
                        ") workers: terrain=", lastTerrainRenderMs,
                        " wmo=", lastWMORenderMs, " m2=", lastM2RenderMs,
                        // Terrain is usually the critical path here, and its cost
                        // is one descriptor bind plus one draw per surviving
                        // chunk - so the counts say whether a slow frame is draw
                        // volume or something else entirely.
                        " | terrain chunks drawn=",
                        terrainRenderer ? terrainRenderer->getRenderedChunkCount() : 0,
                        " culled=",
                        terrainRenderer ? terrainRenderer->getCulledChunkCount() : 0,
                        " resident=",
                        terrainRenderer ? terrainRenderer->getChunkCount() : 0);
        }

        // --- Execute all secondary buffers in correct draw order ---
        VkCommandBuffer validCmds[NUM_SECONDARIES];
        const char* validLabels[NUM_SECONDARIES];
        uint32_t numCmds = 0;
        // Terrain first, then the sky. Every sky layer sits on the far plane
        // and depth-tests against what is already there, so drawing it after
        // the ground skips the clouds' noise on every pixel a hill covers,
        // which outdoors is most of them. Only the terrain goes ahead of it:
        // it is the one pass that is opaque throughout. Buildings carry
        // blended windows and doodads carry leaves and particles, and blended
        // pixels leave no depth behind, so a sky drawn after them would paint
        // over whichever of them stood against it.
        const auto queue = [&](VkCommandBuffer buffer, const char* label) {
            validLabels[numCmds] = label;
            validCmds[numCmds++] = buffer;
        };
        if (drawTerrain || drawGrass)
            queue(secondaryCmds_[SEC_TERRAIN][frameIdx], "terrain");
        queue(secondaryCmds_[SEC_SKY][frameIdx], "sky");
        if (wmoRenderer && frameCamera() && !skipWMO)
            queue(secondaryCmds_[SEC_WMO][frameIdx], "wmo");
        queue(secondaryCmds_[SEC_SELECTION][frameIdx], "selection");
        queue(secondaryCmds_[SEC_CHARS][frameIdx], "characters");
        if (m2Renderer && frameCamera() && !skipM2)
            queue(secondaryCmds_[SEC_M2][frameIdx], "m2");
        if (!fadedCharactersAfterWater())
            queue(secondaryCmds_[SEC_CHARS_FADED][frameIdx], "faded characters");
        queue(secondaryCmds_[SEC_POST][frameIdx], "water/effects");

        // Allocate marks on the owner in execution order. Each mark is
        // recorded in its own inherited secondary: primary timestamp/debug
        // commands are forbidden inside a secondary-only subpass.
        for (uint32_t i = 0; i < numCmds; ++i) {
            vkCmdExecuteCommands(currentCmd, 1, &validCmds[i]);
            recordSecondaryMark(i + 1, validLabels[i]);
        }
        // The world, as one mark.
        //
        // Every pass inside it is marked on the single-threaded path below and
        // none of them are here, because these were recorded into secondary
        // buffers. So on the path this machine actually takes, the next mark
        // after shadows was post-process - and the gap to it, which is the
        // whole scene, was being read as the cost of post-processing. 46 of a
        // 50ms frame landed under a label that did not earn it.
        // ...and the whole of it, for the one-line answer.
        recordSecondaryMark(NUM_DRAW_SECONDARIES + 1, "world total");

    } else {
        // ── Fallback: single-threaded inline recording (original path) ──

        if (terrainRenderer && frameCamera() && frameInputs_.terrainEnabled && !skipTerrain) {
            auto terrainStart = std::chrono::steady_clock::now();
            {
                GpuPass pass(*vkCtx, currentCmd, "terrain");
                if(distantTerrain_)distantTerrain_->record(currentCmd,perFrameSet,frameInputs_.scene->distantTerrain);
                terrainRenderer->recordView(currentCmd, perFrameSet, frameInputs_.scene->terrain);
            }
            lastTerrainRenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - terrainStart).count();
        }

        // After terrain, before the world's models: grass sits on the ground and
        // is occluded by everything standing on it.
        if (grassRenderer_ && vkCtx && !skipGrass) {
            GpuPass pass(*vkCtx, currentCmd, "grass");
            grassRenderer_->record(currentCmd, vkCtx->getCurrentFrame(), perFrameSet, frameInputs_.scene->grass);
        }

        // Sky after the ground, for the reason given on the parallel path.
        if (skySystem && frameCamera() && !skipSky) {
            GpuPass pass(*vkCtx, currentCmd, "sky");
            rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                timeOfDay,
                frameInputs_.environment.gameTime, frameInputs_.weather.strength(),
                frameInputs_.hasLighting ? (&frameInputs_.weatherLight) : nullptr,
                useOriginalSkybox);
            skyParams.sunGlareVisibility =
                frameInputs_.sunGlare * sky_celestial::weatherFade(frameInputs_.weather.strength());
            if (drawSkyModels) skyboxModelRenderer_->prepareRender(frameIdx, *frameCamera());
            skySystem->record(currentCmd, perFrameSet, *frameCamera(), skyParams, frameInputs_.scene->sky,
                              drawSkyModels ? skyboxModelRenderer_.get() : nullptr);
            logSunDiagnostic(skyParams);
        }

        if (wmoRenderer && frameCamera() && !skipWMO) {
            wmoRenderer->prepareRender();
            auto wmoStart = std::chrono::steady_clock::now();
            {
                GpuPass pass(*vkCtx, currentCmd, "wmo");
                wmoRenderer->render(currentCmd, perFrameSet, *frameCamera(), &frameInputs_.characterPosition);
            }
            lastWMORenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wmoStart).count();
        }

        if (overlaySystem_) {
            overlaySystem_->recordSelectionCircle(view, projection, currentCmd, frameInputs_.selection);
        }

        if (characterRenderer && frameCamera() && !skipChars) {
            GpuPass pass(*vkCtx, currentCmd, "characters");
            characterRenderer->render(currentCmd, perFrameSet, *frameCamera(), CharacterRenderer::Part::Solid);
        }

        if (m2Renderer && frameCamera() && !skipM2) {
            m2Renderer->prepareRender(frameIdx, *frameCamera());
            auto m2Start = std::chrono::steady_clock::now();
            {
                GpuPass pass(*vkCtx, currentCmd, "m2");
                m2Renderer->render(currentCmd, perFrameSet, *frameCamera());
                if (!m2Renderer->drawsBlendedAfterWater()) {
                    m2Renderer->renderM2Particles(currentCmd, perFrameSet);
                    m2Renderer->renderM2Ribbons(currentCmd, perFrameSet);
                }
            }
            lastM2RenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m2Start).count();
        }

        // Faded characters over the blended world, unless they go after the
        // water with it.
        if (characterRenderer && frameCamera() && !skipChars && !fadedCharactersAfterWater()) {
            GpuPass pass(*vkCtx, currentCmd, "faded characters");
            characterRenderer->render(currentCmd, perFrameSet, *frameCamera(), CharacterRenderer::Part::Faded);
        }

        if (waterRenderer && frameCamera() && !waterDrawsInContinuePass() && !waterAblated()) {
            GpuPass pass(*vkCtx, currentCmd, "water");
            waterRenderer->setRenderExtent(activeRenderExtent_);
            waterRenderer->recordView(currentCmd, perFrameSet, *frameCamera(), frameInputs_.scene->water, false, frameIdx);
        }
        if (precipitation_ && frameCamera() && !precipitationDrawsWithWater_) {
            precipitation_->record(currentCmd, perFrameSet, frameIdx, frameInputs_.scene->precipitation);
        }
        if (lightning && frameCamera()) lightning->record(currentCmd, perFrameSet, frameInputs_.scene->lightning);
        if (swimEffects && frameCamera() && !swimEffectsDrawWithWater_) {
            swimEffects->record(currentCmd, perFrameSet, frameInputs_.scene->swim);
        }
        if (mountDust && frameCamera()) mountDust->record(currentCmd, perFrameSet, frameInputs_.scene->dust);
        if (chargeEffect && frameCamera()) chargeEffect->record(currentCmd, perFrameSet, frameInputs_.scene->charge);
        if (footprintRenderer && frameCamera()) footprintRenderer->record(currentCmd, perFrameSet, frameInputs_.scene->footprints);
        if (questMarkerRenderer && frameCamera()) questMarkerRenderer->record(currentCmd, perFrameSet, frameInputs_.scene->markers);
        drawGlares(currentCmd, perFrameSet);
        // The same one-line total the parallel path reports, so a profile taken
        // either way can be compared against the other.
        if (vkCtx) vkCtx->gpuMark(currentCmd, "world total");
    }

    // Underwater overlay and minimap - in the fallback path these run inline;
    // in the parallel path they were already recorded into SEC_POST above.
    if (!parallelRecordingEnabled_) {
        renderUnderwaterOverlay(currentCmd);
        renderPostSceneOverlays(currentCmd);
    }

    // Water is drawn last, in a continuation of the scene pass, so that the
    // refraction copy taken just before it holds the scene WITHOUT water. Taking
    // that copy from the finished frame instead fed the water its own output:
    // a moving object left one sharp copy per frame (a train of ghosts) and the
    // brightness applied to the water compounded through the loop and pumped.
    if (waterDrawsInContinuePass() && frameCamera()) {
        vkCmdEndRenderPass(currentCmd);

        VkImage sceneColor = VK_NULL_HANDLE;
        VkImage sceneDepth = VK_NULL_HANDLE;
        VkExtent2D sceneExtent = vkCtx->getSwapchainExtent();
        bool depthIsMsaa = vkCtx->isDepthCopySourceMsaa();
        if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
            sceneColor = postProcessPipeline_->getSceneColorImage();
            sceneDepth = postProcessPipeline_->getSceneDepthImage();
            sceneExtent = postProcessPipeline_->getSceneRenderExtent();
            depthIsMsaa = postProcessPipeline_->sceneDepthIsMsaa();
        } else if (currentImageIndex < vkCtx->getSwapchainImages().size()) {
            sceneColor = vkCtx->getSwapchainImages()[currentImageIndex];
            sceneDepth = vkCtx->getDepthCopySourceImage();
        }

        // The opaque scene is finished and out of its pass: the ray traced
        // lighting reads its depth here, for the surfaces of the next frame.
        recordRtLighting(sceneDepth, sceneExtent, depthIsMsaa, "renderWorld");

        if (sceneColor != VK_NULL_HANDLE && waterRenderer->isRefractionEnabled() && frameInputs_.scene->water.inView &&
            !waterAblated()) {
            waterRenderer->captureSceneHistory(currentCmd, sceneColor, sceneDepth,
                                               sceneExtent, depthIsMsaa,
                                               vkCtx->getCurrentFrame());
        }

        // Without MSAA the water continues into the scene's own framebuffer. With
        // MSAA it draws single-sampled into the resolved image instead, which is
        // both cheaper and what lets it leave the multisampled pass at all.
        const bool msaaOn = vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT;
        VkRenderPassBeginInfo contRp{};
        contRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        VkExtent2D waterExtent = activeRenderExtent_;
        if (msaaOn) {
            contRp.renderPass = waterRenderer->getWater1xRenderPass();
            // Into the image the scene was resolved into: the swapchain's own,
            // or the post-processing target's, which FXAA or the upscaler
            // then read with the water in it.
            const bool offscreenScene = postProcessPipeline_ &&
                postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
            if (offscreenScene) {
                // Ready, or waterDrawsInContinuePass would not have sent the
                // water here.
                contRp.framebuffer = offscreenWater1xFb_;
                waterExtent = postProcessPipeline_->getSceneRenderExtent();
            } else {
                contRp.framebuffer = waterRenderer->getWater1xFramebuffer(currentImageIndex);
                waterExtent = vkCtx->getSwapchainExtent();
            }
        } else {
            contRp.renderPass = vkCtx->getSceneContinueRenderPass();
            contRp.framebuffer = activeFramebuffer_;
        }
        contRp.renderArea.extent = waterExtent;
        vkCmdBeginRenderPass(currentCmd, &contRp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.width = static_cast<float>(waterExtent.width);
        vp.height = static_cast<float>(waterExtent.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = waterExtent;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        waterRenderer->setRenderExtent(waterExtent);
        if (!waterAblated()) {
            waterRenderer->recordView(currentCmd, perFrameSet, *frameCamera(), frameInputs_.scene->water, msaaOn, frameIdx);
        }

        // What is blended goes over the water, not under it. Drawn in the scene
        // pass it was in the refraction copy and then covered by the sheet, so
        // a waterfall's foam was faint and shifted over the pool and at full
        // strength only where it hung past the edge.
        if (m2Renderer && !skipM2) m2Renderer->renderAfterWater(currentCmd, perFrameSet);
        // Faded characters over what was just drawn. A Shadowform priest's
        // fire rises from the feet through the body; with the body drawn
        // first, its depth hid every part of the fire inside or behind it.
        if (characterRenderer && !skipChars && fadedCharactersAfterWater()) {
            characterRenderer->render(currentCmd, perFrameSet, *frameCamera(), CharacterRenderer::Part::Faded);
        }

        // Spray belongs on top of the surface it is thrown off. Recorded in the
        // scene pass it went under the water instead, which the sheet then hid -
        // barely at the shore where alpha sits near its floor, completely in the
        // deeper water you swim in.
        if (swimEffects && frameCamera() && swimEffectsDrawWithWater_) {
            swimEffects->record(currentCmd, perFrameSet, frameInputs_.scene->swim);
        }
        // Rain over the water, ringing on it, not hidden under its sheet.
        if (precipitation_ && precipitationDrawsWithWater_) precipitation_->record(currentCmd, perFrameSet, frameIdx, frameInputs_.scene->precipitation);

        // And the minimap, last of all: it is the interface rather than the
        // world, and nothing in the world belongs over it.
        if (minimapDrawsWithWater_) renderMinimapOverlay(currentCmd);
    }

    auto renderEnd = std::chrono::steady_clock::now();
    lastRenderMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
}

// Water can leave the scene pass only when there is a continuation pass to draw
// it in, which excludes MSAA. Everything else in the frame is unaffected.
void Renderer::syncSwimEffectsTargetPass() {
    if (!vkCtx) return;

    // Default: they stay in the scene pass, matching its MSAA sample count.
    VkRenderPass pass = vkCtx->getImGuiRenderPass();
    VkSampleCountFlagBits samples = vkCtx->getMsaaSamples();
    swimEffectsDrawWithWater_ = false;
    minimapDrawsWithWater_ = false;

    if (waterDrawsInContinuePass()) {
        // Both continuation passes are single-sampled: with MSAA the water draws
        // into the resolved image, and without it there is nothing to resolve.
        if (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
            pass = waterRenderer->getWater1xRenderPass();
        } else {
            pass = vkCtx->getSceneContinueRenderPass();
        }
        if (pass != VK_NULL_HANDLE) {
            samples = VK_SAMPLE_COUNT_1_BIT;
            swimEffectsDrawWithWater_ = true;
            minimapDrawsWithWater_ = true;
        } else {
            pass = vkCtx->getImGuiRenderPass();
        }
    }

    if (swimEffects) swimEffects->setTargetPass(pass, samples);
    // Rain is blended over the world like the spray, and goes with it.
    if (precipitation_) precipitation_->setTargetPass(pass, samples);
    // Blended doodads follow the water out of the scene pass for the same
    // reason. Built by refreshSwimEffectsPass, which retires the old pipelines.
    blendedAfterWaterPass_ = swimEffectsDrawWithWater_ ? pass : VK_NULL_HANDLE;
    // The minimap for the same reason as the spray, and it is the more visible
    // of the two: it is a fixed disc in the corner of the screen, so any water
    // on screen behind it painted straight over the terrain it draws. The
    // spray at least only lost against water it was thrown off.
    //
    // Only when water has actually left the scene pass. Where it has not - no
    // continuation pass, or MSAA without a resolved depth to test against -
    // water is drawn before this and the minimap is already on top of it.
    if (minimap) {
        minimap->setTargetPass(minimapDrawsWithWater_ ? pass : VK_NULL_HANDLE,
                               samples);
    }
}

std::optional<float> Renderer::cameraWaterDepth() const {
    if (!camera) return std::nullopt;
    const glm::vec3 camPos = camera->getPosition();
    if (liquidScene_.isWmoLiquidAt(camPos.x, camPos.y)) return std::nullopt;
    // Far enough for deep ocean; see renderUnderwaterOverlay for what the
    // default 15 units did.
    constexpr float kUnderwaterReach = 400.0f;
    const auto waterH = liquidScene_.nearestHeightAt(
        camPos.x, camPos.y, camPos.z, kUnderwaterReach);
    if (!waterH) return std::nullopt;
    // That reach also finds a lake over a dry cave; the ground tells the two
    // apart. See camera_water_depth.hpp.
    const auto groundH = collision_->terrainHeight(camPos.x, camPos.y);
    const bool hole = collision_->terrainHoleAt(camPos.x, camPos.y);
    return rendering::cameraWaterDepth(camPos.z, waterH, groundH, hole);
}

void Renderer::renderUnderwaterOverlay(VkCommandBuffer cmd) {
    // One implementation, called from both recording paths.
    //
    // There were two. The parallel path had this one - a waterline that
    // sweeps across the view as the eye crosses the surface - and the
    // fallback path had an older one that waited until the eye was 1.5
    // units under before tinting anything at all. Whichever path the
    // client happened to be recording with decided which of the two the
    // player got, and on the fallback path crossing the surface showed a
    // bare line with no water either side of it.
if (overlaySystem_ && frameCamera()) {
        glm::vec3 camPos = frameCamera()->getPosition();
        // The default vertical reach of this query is 15 units, meant to
        // stop water on a cliff above being mistaken for water the camera
        // is in. For the underwater tint that cap is the wrong end of the
        // problem: past 15 units down the query found nothing, the
        // overlay stopped, and the scene snapped bright at a fixed depth.
        // Deep ocean is far deeper than that, so reach much further here.
        const auto waterH = frameInputs_.waterHeight;
        // How far the eye is under the surface. The tint used to wait
        // until 1.5 units down and then apply to the whole screen at
        // once, so crossing the surface was a step: no tint, no tint,
        // fully tinted. Start it at the surface and let a waterline
        // sweep up the view over the crossing instead.
        // How wide the crossing really is: the half-height of the near plane in
        // world units, because that is exactly the slab of world the near plane
        // spans and therefore the only depth range over which part of it can be
        // above the surface while the rest is under. It was a flat 0.55, which
        // is a number rather than a measurement - too wide here, and wrong the
        // moment the field of view or the near plane changes.
        const float fovY = glm::radians(frameCamera()->getFovDegrees());
        const float kCrossingBand =
            std::max(0.05f, frameCamera()->getNearPlane() * std::tan(fovY * 0.5f));
        const float eyeDepth = waterH ? (*waterH - camPos.z) : -1.0f;

        // Says what it decided, so a screenshot of this can be read rather than
        // guessed at. Throttled: it is one line every few seconds, and only
        // while the eye is anywhere near the surface.
        {
            static double lastLog = 0.0;
            if (waterH && std::abs(eyeDepth) < 3.0f && (frameInputs_.time - lastLog) > 2.0) {
                lastLog = frameInputs_.time;
                LOG_INFO("underwater: camZ=", camPos.z, " waterZ=", *waterH,
                         " eyeDepth=", eyeDepth, " band=", kCrossingBand,
                         " wmoWater=", frameInputs_.waterIsWmo ? 1 : 0,
                         " drawing=", (eyeDepth > 0.0f) ? 1 : 0);
            }
        }
        // From a near plane's half-height above the surface, not from the
        // surface itself.
        //
        // Above the water the near plane still cuts the surface, and the water
        // in front of that cut is not drawn at all - which is the hard band of
        // bare lake bed along the bottom of the view when standing in a lake
        // looking across it. Those pixels are looking through water and should
        // be shaded as such.
        //
        // This is only safe because the split is geometric now. It was tried
        // once against the old horizon line and had to be pulled: that test
        // could not tell a dry pixel from a wet one, so standing beside a lake
        // tinted the lower half of the view. The per-pixel test can - a pixel
        // whose ray enters the world above the surface comes out untouched -
        // so the band above the surface costs nothing where there is no water.
        if (waterH && eyeDepth > -kCrossingBand
                   && !frameInputs_.waterIsWmo) {
            const bool canal = frameInputs_.canal;
            // Until the eye passes the surface the view is darkened by
            // looking through the water plane itself, which is strong -
            // its alpha runs up towards 0.9 with depth. Once the eye is
            // under, that plane is behind the camera and contributes
            // nothing, so this overlay is all that is left. Starting it
            // near zero made submerging brighten the scene sharply, which
            // is backwards. Begin at a strength comparable to what the
            // surface was contributing and deepen from there.
            const float depth = std::max(eyeDepth, 0.0f);
            constexpr float kSurfaceHandoff = 0.38f;  // matches the plane's own darkening
            const float depthFog = 1.0f - std::exp(-depth * (canal ? 0.25f : 0.12f));
            float fogStrength = kSurfaceHandoff + depthFog * (0.75f - kSurfaceHandoff);
            fogStrength = glm::clamp(fogStrength, kSurfaceHandoff, 0.75f);
            glm::vec4 tint = canal
                ? glm::vec4(0.01f, 0.04f, 0.10f, fogStrength)
                : glm::vec4(0.03f, 0.09f, 0.18f, fogStrength);

            // The seam is worked out per pixel from the surface height, so
            // there is no screen-space line to place here. Once the eye is
            // well under, the near plane is entirely below the water and there
            // is no seam left to draw - say so, and the whole view tints.
            const bool crossing = eyeDepth < kCrossingBand;
            overlaySystem_->renderWaterline(
                tint,
                glm::inverse(frameCamera()->getViewProjectionMatrix()),
                *waterH,
                // Softness and ripple in world units now: how thick the seam is
                // in yards of water, which does not change with where the
                // camera is looking.
                0.030f,   // meniscus half-thickness
                0.004f,   // ripple on the surface height
                frameInputs_.time, crossing, cmd);
        }
    }
}

void Renderer::renderPostSceneOverlays(VkCommandBuffer cmd) {
    // Ghost mode desaturation: cold blue-grey overlay when dead/ghost
    if (frameInputs_.environment.ghost && overlaySystem_) {
        overlaySystem_->renderOverlay(glm::vec4(0.30f, 0.35f, 0.42f, 0.45f), cmd);
    }

    // Brightness overlay, applied before the minimap so it doesn't affect UI.
    if (overlaySystem_) {
        float br = postProcessPipeline_ ? frameInputs_.postProcess.brightness : 1.0f;
        if (br < 0.99f) {
            // Black overlay at alpha (1-br) darkens as scene*br (a true multiply).
            overlaySystem_->renderOverlay(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f - br), cmd);
        } else if (br > 1.01f) {
            // Multiply scene by br instead of lerping to white (washout). The
            // water refraction shader divides br back out of its captured
            // scene sample so this doesn't compound through the history.
            overlaySystem_->renderBrightnessScale(br, cmd);
        }
    }

    // Unless it is following water into the pass after this one, where it is
    // drawn instead - see renderMinimapOverlay's caller below the water.
    if (!minimapDrawsWithWater_) renderMinimapOverlay(cmd);
}

/// The minimap disc, over whatever has been drawn so far.
void Renderer::renderMinimapOverlay(VkCommandBuffer cmd) {
    if (minimap && frameInputs_.scene) minimap->record(cmd, frameInputs_.scene->minimap);
}

bool Renderer::fadedCharactersAfterWater() const {
    return characterRenderer && characterRenderer->fadedPass() != VK_NULL_HANDLE &&
           waterDrawsInContinuePass();
}

bool Renderer::waterDrawsInContinuePass() const {
    if (!waterRenderer || !vkCtx) return false;
    if (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
        if (!waterRenderer->hasWater1xPass()) return false;
        // With the scene in an off-screen post-processing target (FXAA, FSR)
        // the 1x pass draws into that target's resolved images instead of the
        // swapchain's. It used to be unable to, and the water then stayed in
        // the scene pass with its refraction copy taken from the finished
        // frame - its own last output, so everything standing in or over the
        // water left a trail of copies behind it as the camera moved.
        const bool offscreenScene =
            postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
        return !offscreenScene || offscreenWater1xReady();
    }
    return vkCtx->getSceneContinueRenderPass() != VK_NULL_HANDLE;
}

bool Renderer::offscreenWater1xReady() const {
    if (!offscreenWater1xFb_ || !waterRenderer || !postProcessPipeline_) return false;
    return offscreenWater1xGeneration_ == postProcessPipeline_->sceneTargetGeneration() &&
           offscreenWater1xPass_ == waterRenderer->getWater1xRenderPass() &&
           offscreenWater1xColor_ == postProcessPipeline_->getSceneResolvedColorView() &&
           offscreenWater1xDepth_ == postProcessPipeline_->getSceneResolvedDepthView();
}

void Renderer::updateOffscreenWater1xFramebuffer() {
    if (!waterRenderer || !vkCtx || !postProcessPipeline_) return;
    if (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT) return;
    if (offscreenWater1xReady()) return;
    destroyOffscreenWater1xFramebuffer();

    const VkRenderPass pass = waterRenderer->getWater1xRenderPass();
    const VkImageView color = postProcessPipeline_->getSceneResolvedColorView();
    const VkImageView depth = postProcessPipeline_->getSceneResolvedDepthView();
    const uint64_t generation = postProcessPipeline_->sceneTargetGeneration();
    if (!pass || postProcessPipeline_->getSceneFramebuffer() == VK_NULL_HANDLE) return;
    if (!color || !depth) {
        if (offscreenWater1xFailedGeneration_ != generation) {
            offscreenWater1xFailedGeneration_ = generation;
            LOG_WARNING("Water: the off-screen scene has no resolved depth - water stays in the "
                        "multisampled scene pass and refracts the previous frame (trails behind "
                        "anything moving over it)");
        }
        return;
    }

    const VkExtent2D extent = postProcessPipeline_->getSceneRenderExtent();
    const VkImageView attachments[2] = {color, depth};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = pass;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = attachments;
    fbInfo.width = extent.width;
    fbInfo.height = extent.height;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(vkCtx->getDevice(), &fbInfo, nullptr, &offscreenWater1xFb_) != VK_SUCCESS) {
        offscreenWater1xFb_ = VK_NULL_HANDLE;
        if (offscreenWater1xFailedGeneration_ != generation) {
            offscreenWater1xFailedGeneration_ = generation;
            LOG_ERROR("Water: could not build its pass over the off-screen scene (", extent.width,
                      "x", extent.height, ") - water stays in the multisampled scene pass and "
                      "refracts the previous frame");
        }
        return;
    }
    offscreenWater1xGeneration_ = generation;
    offscreenWater1xPass_ = pass;
    offscreenWater1xColor_ = color;
    offscreenWater1xDepth_ = depth;
}

void Renderer::destroyOffscreenWater1xFramebuffer() {
    if (!offscreenWater1xFb_ || !vkCtx) return;
    // A frame still in flight may have recorded it.
    VkDevice device = vkCtx->getDevice();
    VkFramebuffer fb = offscreenWater1xFb_;
    vkCtx->deferAfterAllFrameFences([device, fb]() { vkDestroyFramebuffer(device, fb, nullptr); });
    offscreenWater1xFb_ = VK_NULL_HANDLE;
}

// initPostProcess(), resizePostProcess(), shutdownPostProcess() removed -
// post-process pipeline is now handled by Vulkan (Phase 6 cleanup).

void Renderer::setActiveMapName(const std::string& name) {
    if (terrainManager) terrainManager->setMapName(name);
    if (minimap) minimap->setMapName(name);
}

bool Renderer::initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName) {
    drainFrames();
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Initializing renderers for map: ", mapName);

    // Scan for custom zones on first initialization
    if (customZones_.empty()) {
        customZones_ = pipeline::CustomZoneDiscovery::scan({"custom_zones", "output"});
        if (!customZones_.empty()) {
            LOG_INFO("=== Custom Zones Available ===");
            for (const auto& z : customZones_) {
                LOG_INFO("  ", z.name, " (", z.directory, ")",
                         z.hasCreatures ? " [NPCs]" : "",
                         z.hasQuests ? " [Quests]" : "");
            }
            LOG_INFO("==============================");
        }
    }

    // Create terrain renderer if not already created
    if (!terrainRenderer) {
        terrainRenderer = std::make_unique<TerrainRenderer>();
        if (!terrainRenderer->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_ERROR("Failed to initialize terrain renderer");
            terrainRenderer.reset();
            return false;
        }
        terrainRenderer->setRtScene(rtScene_.get());
        if (shadowRenderPass != VK_NULL_HANDLE) {
            terrainRenderer->initializeShadow(shadowRenderPass);
        }
    } else if (!terrainRenderer->hasShadowPipeline() && shadowRenderPass != VK_NULL_HANDLE) {
        terrainRenderer->initializeShadow(shadowRenderPass);
    }

    // The sun's disc and glare textures, from the client's data.
    if (skySystem) skySystem->loadTextures(assetManager);

    // What each liquid id in the map data is, read once: the scene says what
    // kind a liquid is by it, and the water finds each liquid's ripple frames
    // by it. Before any tile or building is loaded.
    if (!liquidTypesLoaded_) {
        liquidTypesLoaded_ = true;
        const auto dbc = assetManager->loadDBC("LiquidType.dbc");
        const pipeline::DBCLayout* layouts = pipeline::getActiveDBCLayout();
        if (dbc && dbc->isLoaded()) {
            liquidTypes_ = pipeline::LiquidTypeTable::fromDbc(
                *dbc, layouts ? layouts->getLayout("LiquidType") : nullptr, "LiquidType.dbc");
            LOG_INFO("Liquid types: ", liquidTypes_.size(), " from LiquidType.dbc");
        } else {
            LOG_WARNING("Liquid types: LiquidType.dbc did not load - only the four basic "
                        "liquids are known, drawn without their textures");
            liquidTypes_ = pipeline::LiquidTypeTable::basicOnly();
        }
        liquidScene_.setLiquidTypes(&liquidTypes_);
    }

    // What lies under the water, drawn by the shadow pass's casters: before
    // the water, whose sets bind it.
    if (!seaBed_) {
        seaBed_ = std::make_unique<SeaBedMap>();
        if (!seaBed_->initialize(vkCtx, shadowRenderPass)) {
            LOG_ERROR("Failed to create the sea bed map - liquid is not drawn, "
                      "though it can still be swum in");
        }
    }

    // Where rain and snow cannot reach, drawn by the same casters, and what
    // falls, which reads it.
    if (!shelter_) {
        shelter_ = std::make_unique<ShelterMap>();
        if (!shelter_->initialize(vkCtx, shadowRenderPass)) {
            shelter_->shutdown();
            shelter_.reset();
        }
    }
    if (!precipitation_ && shelter_) {
        precipitation_ = std::make_unique<Precipitation>();
        syncSwimEffectsTargetPass();
        if (!precipitation_->initialize(vkCtx, perFrameSetLayout, *shelter_)) {
            precipitation_->shutdown();
            precipitation_.reset();
        } else {
            precipitationDrawsWithWater_ = swimEffectsDrawWithWater_;
            precipitation_->setDensityScale(weatherDetail_);
        }
    } else if (!shelter_) {
        LOG_ERROR("No shelter map - rain, snow and sand are not drawn");
    }

    // Create water renderer if not already created
    if (!waterRenderer) {
        waterRenderer = std::make_unique<WaterRenderer>();
        if (!waterRenderer->initialize(vkCtx, perFrameSetLayout, seaBed_.get())) {
            LOG_ERROR("Failed to initialize water renderer - liquid is not drawn, "
                      "though it can still be swum in");
            waterRenderer.reset();
        } else {
            // Told of whatever the scene already holds before this returns.
            waterSubscription_ = liquidScene_.subscribe(*waterRenderer);
        }
    }

    // Create minimap if not already created
    if (!minimap) {
        minimap = std::make_unique<Minimap>();
        if (!minimap->initialize(vkCtx, perFrameSetLayout)) {
            LOG_ERROR("Failed to initialize minimap");
            minimap.reset();
        }
    }

    // Create M2, WMO, and Character renderers
    if (!m2Renderer) {
        m2Renderer = std::make_unique<M2Renderer>();
        if (!m2Renderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("M2Renderer initialization failed");
        m2Renderer->setRtScene(rtScene_.get());
        m2Renderer->setCollisionWorld(collision_.get());
        m2Renderer->setWaterHeightQuery([this](float x, float y, float z) -> std::optional<float> {
            return liquidScene_.nearestHeightAt(x, y, z, 3.0f);
        });
        if (swimEffects) {
            swimEffects->setM2Renderer(m2Renderer.get());
        }
    }

    // The original client's skies are camera-centered M2 models selected
    // through LightParams and LightSkybox, on every map that names one - not
    // Outland alone, which is where this was built and where it stayed. The
    // lookup was opened to all maps and this was not, so it went on doing
    // nothing anywhere else: without the renderer there is nothing to draw
    // into.
    //
    // It keeps its own no-depth renderer so the sky draws behind terrain and
    // never enters world collision.
    if (!skyboxModelRenderer_) {
        skyboxModelRenderer_ = std::make_unique<M2Renderer>();
        skyboxModelRenderer_->setSkyMode(true);
        if (!skyboxModelRenderer_->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_WARNING("Sky M2 renderer initialization failed");
            skyboxModelRenderer_.reset();
        }
    }

    // HiZ occlusion culling disabled - the pyramid build + blocking fence was
    // the main frame-rate bottleneck.  GPU frustum culling alone provides good
    // draw-call reduction without the per-frame GPU stall.  HiZ can be re-
    // enabled once the pyramid build is moved to an async compute queue.
    if (!wmoRenderer) {
        wmoRenderer = std::make_unique<WMORenderer>();
        if (!wmoRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("WMORenderer initialization failed");
        wmoRenderer->setRtScene(rtScene_.get());
        wmoRenderer->setCollisionWorld(collision_.get());
        if (shadowRenderPass != VK_NULL_HANDLE) {
            if (!wmoRenderer->initializeShadow(shadowRenderPass))
                LOG_WARNING("WMO shadow pipeline initialization failed");
        }
    }

    // Renderer components can be recreated during map transitions. Restore the
    // configured view distance instead of falling back to their defaults.
    setViewDistance(viewDistance_);
    setSharpStars(sharpStars_);

    // Initialize shadow pipelines for M2 if not yet done
    if (m2Renderer && shadowRenderPass != VK_NULL_HANDLE && !m2Renderer->hasShadowPipeline()) {
        if (!m2Renderer->initializeShadow(shadowRenderPass))
            LOG_WARNING("M2 shadow pipeline initialization failed");
    }
    if (!characterRenderer) {
        characterRenderer = std::make_unique<CharacterRenderer>();
        if (!characterRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("CharacterRenderer initialization failed");
        if (shadowRenderPass != VK_NULL_HANDLE) {
            if (!characterRenderer->initializeShadow(shadowRenderPass))
                LOG_WARNING("Character shadow pipeline initialization failed");
        }
    }
    if (!characterEmitters_ && characterRenderer && m2Renderer) {
        characterEmitters_ = std::make_unique<CharacterEmitters>(*characterRenderer, *m2Renderer);
    }

    // Create and initialize terrain manager
    if (!terrainManager) {
        standaloneTerrain_ = std::make_unique<TerrainManager>();
        terrainManager = standaloneTerrain_.get();
        if (mapJobToken_.current()) terrainManager->setJobToken(mapJobToken_);
        if (!terrainManager->initialize(assetManager, terrainRenderer.get())) {
            LOG_ERROR("Failed to initialize terrain manager");
            standaloneTerrain_.reset();
            terrainManager = nullptr;
            return false;
        }
        // The liquid streams in with the terrain and the buildings, and the
        // ground with the tiles.
        terrainManager->setLiquidScene(&liquidScene_);
        terrainManager->setCollisionWorld(collision_.get());
        // Set M2 renderer for doodad loading during streaming
        if (m2Renderer) {
            terrainManager->setM2Renderer(m2Renderer.get());
        }
        // Set WMO renderer for building loading during streaming
        if (wmoRenderer) {
            terrainManager->setWMORenderer(wmoRenderer.get());
        }
        // A WMO's child M2 doodads - a ship's sails, its paddlewheel - are moved
        // and destroyed through this pointer. It was never set, so every one of
        // those paths was behind a null check that never passed: the doodads
        // were created at the origin, never given their parent's transform, and
        // so drawn at the middle of the map rather than on the ship. Static
        // world doodads were unaffected, because terrain streaming places those
        // at their world position itself and never goes through the parent.
        if (wmoRenderer && m2Renderer) {
            wmoRenderer->setM2Renderer(m2Renderer.get());
        }
        terrainManager->setAmbientEmitterSink(ambientEmitterSink_);
        // Pass asset manager to character renderer for texture loading
        if (characterRenderer) {
            characterRenderer->setAssetManager(assetManager);
        }
        // Wire asset manager to minimap for tile texture loading
        if (minimap) {
            minimap->setAssetManager(assetManager);
        }
    }

    // Set map name on sub-renderers
    setActiveMapName(mapName);

    // The renderers that load their art on first entering the world - now,
    // or over the first frames (see runDeferredWorldInitStep).
    if (assetManager && !cachedAssetManager) {
        if (!deferredWorldInitEnabled_) {
            if (questMarkerRenderer) {
                if (!questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
                    LOG_WARNING("Quest marker renderer initialization failed (non-fatal)");
            }
            if (footprintRenderer) {
                if (!footprintRenderer->initialize(this, vkCtx, perFrameSetLayout, assetManager))
                    LOG_WARNING("Footprint renderer initialization failed (non-fatal)");
            }
        } else {
            deferredWorldInitPending_ = true;
            deferredWorldInitStage_ = 0;
            deferredWorldInitCooldown_ = 0.25f;
        }

        cachedAssetManager = assetManager;
    }

    return true;
}

bool Renderer::loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath) {
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Loading test terrain: ", adtPath);

    // Extract map name from ADT path for renderer initialization
    std::string mapName;
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            mapName = filename.substr(0, firstUnderscore != std::string::npos ? firstUnderscore : filename.size());
        }
    }

    // Initialize all sub-renderers
    if (!initializeRenderers(assetManager, mapName)) {
        return false;
    }

    // Parse tile coordinates from ADT path
    // Format: World\Maps\{MapName}\{MapName}_{X}_{Y}.adt
    int tileX = 32, tileY = 49;  // defaults
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            if (firstUnderscore != std::string::npos) {
                size_t secondUnderscore = filename.find('_', firstUnderscore + 1);
                if (secondUnderscore != std::string::npos) {
                    size_t dot = filename.find('.', secondUnderscore);
                    if (dot != std::string::npos) {
                        try {
                            tileX = std::stoi(filename.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1));
                            tileY = std::stoi(filename.substr(secondUnderscore + 1, dot - secondUnderscore - 1));
                        } catch (...) {
                            LOG_WARNING("Failed to parse tile coords from: ", filename);
                        }
                    }
                }
            }
        }
    }

    LOG_INFO("Enqueuing initial tile [", tileX, ",", tileY, "] via terrain manager");

    // Enqueue the initial tile for async loading (avoids long sync stalls)
    if (!terrainManager->enqueueTile(tileX, tileY)) {
        LOG_ERROR("Failed to enqueue initial tile [", tileX, ",", tileY, "]");
        return false;
    }

    terrainLoaded = true;

    LOG_INFO("Test terrain loaded successfully!");
    LOG_INFO("  Chunks: ", terrainRenderer->getChunkCount());
    LOG_INFO("  Triangles: ", terrainRenderer->getTriangleCount());

    return true;
}

void Renderer::setWireframeMode(bool enabled) {
    if (terrainRenderer) {
        terrainRenderer->setWireframe(enabled);
    }
}

// One line naming how far each of the three actually drew this frame.
//
// "Distant objects float with no terrain" is a disagreement between two
// distances, and every attempt to settle it by reading the code picked the
// wrong one of the three places that compute it. This prints the answer:
// if terrain stops short of the doodads the fault is in loading the tiles,
// and if the doodads run past the setting the fault is in the cull. It is at
// warning because the default log is warnings only, and it prints on a change
// of half a tile rather than every frame.
void Renderer::logViewDistanceDiag() {
    static const bool enabled = std::getenv("WOWEE_VIEW_DIAG") != nullptr;
    if (!enabled) return;

    const float terrainFurthest = terrainRenderer
        ? terrainRenderer->getFurthestDrawnDistance() : 0.0f;
    const float m2Furthest = m2Renderer
        ? m2Renderer->getFurthestDrawnDistance() : 0.0f;

    if (std::abs(terrainFurthest - diagTerrainFurthest_) < 266.0f &&
        std::abs(m2Furthest - diagM2Furthest_) < 266.0f) {
        return;
    }
    diagTerrainFurthest_ = terrainFurthest;
    diagM2Furthest_ = m2Furthest;

    LOG_WARNING("view distance ", static_cast<int>(frameInputs_.viewDistance),
                ": terrain drew to ", static_cast<int>(terrainFurthest),
                ", doodads to ", static_cast<int>(m2Furthest),
                ", tiles loaded to ", getTerrainLoadRadius(),
                " (", static_cast<int>(getTerrainLoadRadius() *
                                       core::coords::TILE_SIZE), ")");
}

void Renderer::setSharpStars(bool enabled) {
    sharpStars_ = enabled;
}

void Renderer::setViewDistance(float distance) {
    viewDistance_ = glm::clamp(distance, kMinViewDistance, kMaxViewDistance);

    if (terrainManager) {
        terrainManager->setLandmarkRadius(distantTerrain_ && distantTerrain_->hasSource() ? static_cast<int>(std::ceil(viewDistance_/core::coords::TILE_SIZE))+1 : 0);
        terrainManager->setLoadRadius(getTerrainLoadRadius());
        terrainManager->setUnloadRadius(getTerrainUnloadRadius());
    }
}

int Renderer::getTerrainLoadRadius() const {
    constexpr float kAdtTileSize = core::coords::TILE_SIZE;
    // One tile beyond the visible circle covers the camera's position inside
    // its tile. Derive the radius from the setting instead of capping it at
    // six tiles, which would leave the far end of a long view unloaded.
    const float detailedRange=distantTerrain_ && distantTerrain_->hasSource() ? std::min(viewDistance_,kDistantTerrainEnd) : viewDistance_;
    return std::max(2, static_cast<int>(std::ceil(detailedRange / kAdtTileSize)) + 1);
}

int Renderer::getTerrainUnloadRadius() const {
    return getTerrainLoadRadius()+(distantTerrain_ && distantTerrain_->hasSource() ? 1 : 3);
}

// ──────────────────────────────────────────────────────
// Shadow mapping helpers
// ──────────────────────────────────────────────────────

// initShadowMap() and compileShadowShader() removed - shadow resources now created
// in createPerFrameResources() as part of the Vulkan shadow infrastructure.

glm::mat4 Renderer::computeLightSpaceMatrix(const FrameInputs& inputs, glm::vec3& shadowCentre) {
    const float kShadowHalfExtent = inputs.shadowDistance;
    const float kShadowLightDistance = inputs.shadowDistance * 3.0f;
    constexpr float kShadowNearPlane = 1.0f;
    const float kShadowFarPlane = inputs.shadowDistance * 6.5f;

    // Use active lighting direction so shadow projection matches main shading.
    // Fragment shaders derive lighting with `ldir = normalize(-lightDir.xyz)`,
    // therefore shadow rays must use -directionalDir to stay aligned.
    glm::vec3 sunDir = glm::normalize(glm::vec3(-0.3f, -0.7f, -0.6f));
    if (inputs.hasLighting) {
        const auto& lighting = inputs.lighting;
        float ldirLenSq = glm::dot(lighting.directionalDir, lighting.directionalDir);
        if (ldirLenSq > 1e-6f) {
            sunDir = -lighting.directionalDir * glm::inversesqrt(ldirLenSq);
        }
    }
    // Shadow camera expects light rays pointing downward in render space (Z up).
    // Some profiles/opcode paths provide the opposite convention; normalize here.
    if (sunDir.z > 0.0f) {
        sunDir = -sunDir;
    }
    // Keep a minimum downward component so the frustum doesn't collapse at grazing angles.
    if (sunDir.z > -0.15f) {
        sunDir.z = -0.15f;
        sunDir = glm::normalize(sunDir);
    }

    // LightingManager already smooths the directional light every frame. Keep
    // that continuous direction for the shadow projection as well. Quantizing
    // it into 0.5-degree steps made the entire 600-yard shadow footprint rotate
    // in a single frame, producing a wide light-switch flicker whenever the
    // threshold was crossed. Translation remains stabilized by texel snapping.

    // Shadow center follows the player directly; texel snapping below prevents
    // camera translation from shimmering the projection.
    glm::vec3 desiredCenter = inputs.characterPosition;
    if (!shadowCenterInitialized) {
        if (glm::dot(desiredCenter, desiredCenter) < 1.0f) {
            return glm::mat4(0.0f);
        }
        shadowCenterInitialized = true;
    }
    shadowCentre = desiredCenter;
    glm::vec3 center = shadowCentre;

    // Snap shadow frustum to texel grid so the projection is perfectly stable
    // while moving. We compute the light's right/up axes from the sun direction
    // (these are constant per frame regardless of center) and snap center along
    // them before building the view matrix.
    float halfExtent = kShadowHalfExtent;
    float texelWorld = (2.0f * halfExtent) / static_cast<float>(SHADOW_MAP_SIZE);

    // Stable light-space axes (independent of center position)
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if (std::abs(glm::dot(sunDir, up)) > 0.99f) {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    glm::vec3 lightRight = glm::normalize(glm::cross(sunDir, up));
    glm::vec3 lightUp = glm::normalize(glm::cross(lightRight, sunDir));

    // Snap center along light's right and up axes to align with texel grid.
    // This eliminates sub-texel shifts that cause shadow shimmer.
    float dotR = glm::dot(center, lightRight);
    float dotU = glm::dot(center, lightUp);
    dotR = std::floor(dotR / texelWorld) * texelWorld;
    dotU = std::floor(dotU / texelWorld) * texelWorld;
    float dotD = glm::dot(center, sunDir);  // depth axis unchanged
    center = lightRight * dotR + lightUp * dotU + sunDir * dotD;
    shadowCentre = center;

    glm::mat4 lightView = glm::lookAt(center - sunDir * kShadowLightDistance, center, up);
    glm::mat4 lightProj = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent,
                                     kShadowNearPlane, kShadowFarPlane);
    lightProj[1][1] *= -1.0f; // Vulkan Y-flip for shadow pass

    return lightProj * lightView;
}

void Renderer::setupWater1xPass() {
    if (!waterRenderer || !vkCtx) return;
    if (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT) {
        refreshSwimEffectsPass();  // scene continuation pass covers the water here
        return;
    }
    VkImageView depthView = vkCtx->getDepthResolveImageView();
    if (!depthView) {
        // Without a resolved depth buffer the single-sampled water has nothing
        // to depth test against, so it has to stay in the multisampled pass.
        LOG_WARNING("No depth resolve image available - water stays in the MSAA scene pass");
        refreshSwimEffectsPass();
        return;
    }

    waterRenderer->createWater1xPass(vkCtx->getSwapchainFormat(), vkCtx->getDepthFormat());
    waterRenderer->createWater1xFramebuffers(
        vkCtx->getSwapchainImageViews(), depthView, vkCtx->getSwapchainExtent());

    // The spray follows the water into its pass, and this is the first point at
    // which that pass exists - the swim effects were built long before it, back
    // when the only choice was the scene pass.
    refreshSwimEffectsPass();
}

// Re-decide where the spray and the minimap draw, and rebuild whichever one
// the answer moved.
//
// Called between frames only: recreatePipelines() destroys the old pipelines
// outright rather than deferring them, so the wait below is what keeps that
// off a frame in flight. Free when nothing changed, which is every frame but
// a handful, so beginFrame runs it too. The decision depends on whether the
// post-process target exists, and that target is created lazily on the frame
// after the anti-aliasing rebuild that first made the decision. With MSAA and
// FXAA together the rebuild chose the single-sample water pass, the FXAA
// target then moved the water back into the scene pass, and the minimap and
// the spray - still flagged for a pass that no longer ran - were drawn
// nowhere at all. The same happened, in either direction, whenever FXAA was
// toggled in the settings.
void Renderer::refreshSwimEffectsPass() {
    if (!vkCtx) return;
    const bool sprayWasWithWater = swimEffectsDrawWithWater_;
    const bool minimapWasWithWater = minimapDrawsWithWater_;
    syncSwimEffectsTargetPass();
    const bool sprayMoved = swimEffects && swimEffectsDrawWithWater_ != sprayWasWithWater;
    const bool minimapMoved = minimap && minimapDrawsWithWater_ != minimapWasWithWater;
    const bool blendedMoved = m2Renderer && m2Renderer->afterWaterPass() != blendedAfterWaterPass_;
    // Faded characters go wherever the blended world goes, after it.
    const bool fadedMoved = characterRenderer && characterRenderer->fadedPass() != blendedAfterWaterPass_;
    if (precipitation_ && precipitationDrawsWithWater_ != swimEffectsDrawWithWater_) {
        precipitation_->recreatePipelines();
        precipitationDrawsWithWater_ = swimEffectsDrawWithWater_;
    }
    if (!sprayMoved && !minimapMoved && !blendedMoved && !fadedMoved) return;
    if (sprayMoved) swimEffects->recreatePipelines();
    if (minimapMoved) minimap->recreatePipelines();
    if (blendedMoved) m2Renderer->setAfterWaterPass(blendedAfterWaterPass_, VK_SAMPLE_COUNT_1_BIT);
    if (fadedMoved) characterRenderer->setFadedPass(blendedAfterWaterPass_, VK_SAMPLE_COUNT_1_BIT);
}

// ========================= Multithreaded Secondary Command Buffers =========================

bool Renderer::createSecondaryCommandResources() {
    if (!vkCtx) return false;
    VkDevice device = vkCtx->getDevice();
    uint32_t queueFamily = vkCtx->getGraphicsQueueFamily();

    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = queueFamily;

    // Create worker command pools (one per worker thread)
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        if (vkCreateCommandPool(device, &poolCI, nullptr, &workerCmdPools_[w]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create worker command pool ", w);
            return false;
        }
    }

    // Create main-thread secondary command pool
    if (vkCreateCommandPool(device, &poolCI, nullptr, &mainSecondaryCmdPool_) != VK_SUCCESS) {
        LOG_ERROR("Failed to create main secondary command pool");
        return false;
    }

    // Allocate secondary command buffers
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = 1;

    // Each concurrently recorded worker secondary owns a dedicated command pool.
    const uint32_t workerSecondaries[] = { SEC_TERRAIN, SEC_WMO, SEC_CHARS, SEC_M2, SEC_POST };
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        allocInfo.commandPool = workerCmdPools_[w];
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[workerSecondaries[w]][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate worker secondary buffer w=", w, " f=", f);
                return false;
            }
            // The faded characters are recorded by the characters' worker,
            // after its own buffer, so they come from its pool.
            if (workerSecondaries[w] == SEC_CHARS &&
                vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[SEC_CHARS_FADED][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate the faded characters' secondary buffer f=", f);
                return false;
            }
        }
    }

    for (uint32_t idx = 0; idx < NUM_SECONDARIES; ++idx) {
        if (idx < NUM_DRAW_SECONDARIES && idx != SEC_SKY && idx != SEC_SELECTION && idx != SEC_IMGUI) continue;
        allocInfo.commandPool = mainSecondaryCmdPool_;
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[idx][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate main secondary buffer idx=", idx, " f=", f);
                return false;
            }
        }
    }

    parallelRecordingEnabled_ = true;
    LOG_INFO("Multithreaded rendering: ", NUM_WORKERS, " worker threads, ",
             NUM_SECONDARIES, " secondary buffers [ENABLED]");
    return true;
}

void Renderer::destroySecondaryCommandResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    vkCtx->waitIdle();

    // Secondary buffers are freed when their pool is destroyed
    for (auto& workerCmdPool : workerCmdPools_) {
        if (workerCmdPool) {
            vkDestroyCommandPool(device, workerCmdPool, nullptr);
            workerCmdPool = VK_NULL_HANDLE;
        }
    }
    if (mainSecondaryCmdPool_) {
        vkDestroyCommandPool(device, mainSecondaryCmdPool_, nullptr);
        mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    }

    for (auto& arr : secondaryCmds_)
        for (auto& cmd : arr)
            cmd = VK_NULL_HANDLE;

    parallelRecordingEnabled_ = false;
}

void Renderer::recordSecondaryMark(uint32_t index, const char* label) {
    auto cmd = beginSecondary(NUM_DRAW_SECONDARIES + index);
    // Query allocation stays on the recording owner and follows execution order.
    // These buffers contain no draws and inherit the same pass as the workers.
    vkCtx->gpuMark(cmd, label);
    vkEndCommandBuffer(cmd);
    vkCmdExecuteCommands(currentCmd, 1, &cmd);
}

VkCommandBuffer Renderer::beginSecondary(uint32_t secondaryIndex) {
    uint32_t frame = vkCtx->getCurrentFrame();
    VkCommandBuffer cmd = secondaryCmds_[secondaryIndex][frame];

    VkCommandBufferInheritanceInfo inheritInfo{};
    inheritInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritInfo.renderPass = activeRenderPass_;
    inheritInfo.subpass = 0;
    inheritInfo.framebuffer = activeFramebuffer_;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                    | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    beginInfo.pInheritanceInfo = &inheritInfo;

    VkResult result = vkBeginCommandBuffer(cmd, &beginInfo);
    if (result != VK_SUCCESS) {
        LOG_ERROR("vkBeginCommandBuffer failed for secondary ", secondaryIndex,
                  " frame ", frame, " result=", static_cast<int>(result));
    }
    return cmd;
}

void Renderer::setSecondaryViewportScissor(VkCommandBuffer cmd) {
    VkViewport vp{};
    vp.width = static_cast<float>(activeRenderExtent_.width);
    vp.height = static_cast<float>(activeRenderExtent_.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = activeRenderExtent_;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

bool Renderer::waterAblated() const {
    return passAblation_ && passAblation_->skip(AblationPass::Water);
}

bool Renderer::waterReflectionAblated() const {
    return waterAblated() || (passAblation_ && passAblation_->skip(AblationPass::WaterReflection));
}

void Renderer::renderReflectionPass() {
    if (!waterRenderer || !frameCamera() || !waterRenderer->hasReflectionPass() || !frameInputs_.scene->water.inView) return;
    if (waterReflectionAblated()) return;
    // Select the current frame's pre-bound reflection descriptor set
    // (each frame's set was bound to its own UBO and shadow view at init).
    uint32_t frame = vkCtx->getCurrentFrame();
    if (currentCmd == VK_NULL_HANDLE || !reflPerFrameUBOMapped[frame]) return;
    VkDescriptorSet reflDescSet = reflPerFrameDescSet[frame];

    // Reflection pass uses 1x MSAA. Scene pipelines must be render-pass-compatible,
    // which requires matching sample counts. Only render scene into reflection when MSAA is off.
    bool canRenderScene = (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT);

    // Find dominant water height near camera
    const glm::vec3 camPos = frameCamera()->getPosition();
    auto waterH = frameInputs_.reflectionWaterHeight;
    if (!waterH) return;

    float waterHeight = *waterH;

    // Skip reflection if camera is underwater (Z is up)
    if (camPos.z < waterHeight + 0.5f) return;

    // Compute reflected view and oblique projection
    glm::mat4 reflView = mirroredView(frameCamera()->getPosition(), frameCamera()->getForward(), waterHeight);
    glm::mat4 reflProj = obliqueReflectionProjection(frameCamera()->getProjectionMatrix(), reflView, waterHeight);

    // Update water renderer's reflection UBO with the reflected viewProj
    waterRenderer->updateReflectionUBO(reflProj * reflView);

    // Fill the reflection per-frame UBO (same as normal but with reflected matrices)
    GPUPerFrameData reflData = currentFrameData;
    reflData.view = reflView;
    reflData.projection = reflProj;
    // Reflected camera position (Z is up)
    glm::vec3 reflPos = camPos;
    reflPos.z = 2.0f * waterHeight - reflPos.z;
    reflData.viewPos = glm::vec4(reflPos, 1.0f);
    // The fog volume is the camera's; its sets here bind the neutral one.
    reflData.volumetricParams = glm::vec4(0.0f);
    reflData.rtParams = glm::vec4(0.0f);
    reflData.fogSkyMiddle.w = 0.0f; // the main camera sky image is not a reflected view
    std::memcpy(reflPerFrameUBOMapped[frame], &reflData, sizeof(GPUPerFrameData));

    // Begin reflection render pass (clears to black; scene rendered if pipeline-compatible)
    if (!waterRenderer->beginReflectionPass(currentCmd)) return;

    if (canRenderScene) {
        // What stands above the water: the ground and the buildings. Not the
        // sky - the water reflects that itself, along each pixel's own
        // reflected ray, from the sky the camera sees (skyAlong in
        // water.frag). The sky drawn here was a procedural one with no sky
        // model and no weather, and where the reflection held it, it stood
        // in for the real one as a grey wash over the sea.
        if (terrainRenderer && frameInputs_.terrainEnabled) {
            if(distantTerrain_)distantTerrain_->record(currentCmd,reflDescSet,frameInputs_.scene->distantTerrain);
            terrainRenderer->recordView(currentCmd, reflDescSet, frameInputs_.scene->terrain);
        }
        if (wmoRenderer) {
            wmoRenderer->render(currentCmd, reflDescSet, *frameCamera(), nullptr, WMORenderer::Pass::Reflection);
        }
    }

    waterRenderer->endReflectionPass(currentCmd);
}

void Renderer::renderShadowPass() {
    {
        GpuScope scope(*vkCtx, currentCmd, "shadows");
        renderShadowMap(ShadowView::Sun);
    }
    {
        GpuScope scope(*vkCtx, currentCmd, "contact shadows");
        renderShadowMap(ShadowView::Contact);
    }
    if (currentFrameData.lightningColor.b > 0.0f) {
        GpuScope scope(*vkCtx, currentCmd, "lightning shadows");
        renderShadowMap(ShadowView::Lightning);
    }
}

void Renderer::renderShadowMap(ShadowView view) {
    const bool contact = view == ShadowView::Contact;
    const bool strike = view == ShadowView::Lightning;
    ZoneScopedN("Renderer::renderShadowPass");
    static const bool skipShadows = (std::getenv("WOWEE_SKIP_SHADOWS") != nullptr);
    if (!strike && skipShadows) return;
    if (!strike && passAblation_ && passAblation_->skip(AblationPass::Shadows)) return;
    if (shadowDepthImage[0] == VK_NULL_HANDLE) return;
    if (currentCmd == VK_NULL_HANDLE) return;
    // Shadows off still runs the pass, and the pass still clears the map and
    // leaves it in the layout its readers expect - it simply draws nothing
    // into it. Returning here instead left the image untransitioned while it
    // stayed bound for sampling, which is the shape of fault that takes the
    // device down rather than drawing something wrong.
    const bool drawCasters = strike || (frameInputs_.shadows && (!contact || currentFrameData.contactParams.x > 0.5f));

    // Shadows render every frame - throttling causes visible flicker on player/NPCs

    const glm::mat4& passMatrix = strike ? currentFrameData.lightningMatrix : contact ? currentFrameData.contactSpaceMatrix : lightSpaceMatrix;
    const uint32_t mapSize = strike ? LIGHTNING_MAP_SIZE : contact ? CONTACT_MAP_SIZE : SHADOW_MAP_SIZE;
    // passMatrix was already computed at frame start (before updatePerFrameUBO).
    // Zero matrix means character position isn't set yet - skip shadow pass entirely.
    if (passMatrix == glm::mat4(0.0f)) return;
    uint32_t frame = vkCtx->getCurrentFrame();
    const VkImage passImage = strike ? lightningShadowImages_[frame].image : contact ? contactShadowImages_[frame].image : shadowDepthImage[frame];
    const VkImageView passView = strike ? lightningShadowImages_[frame].imageView : contact ? contactShadowImages_[frame].imageView : shadowDepthView[frame];
    const VkFramebuffer passFramebuffer = strike ? lightningShadowFramebuffer_[frame] : contact ? contactShadowFramebuffer_[frame] : shadowFramebuffer[frame];
    auto& passLayout = strike ? lightningShadowLayout_[frame] : contact ? contactShadowLayout_[frame] : shadowDepthLayout_[frame];
    const glm::vec3 passCenter = strike ? frameInputs_.scene->lightning.ground : contact ? frameInputs_.characterPosition : shadowCenter;

    // Barrier 1: transition this frame's shadow map into writable depth layout.
    VkImageMemoryBarrier2 b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b1.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    b1.oldLayout = passLayout;
    b1.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.srcAccessMask = (passLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? VK_ACCESS_SHADER_READ_BIT
        : 0;
    b1.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b1.image = passImage;
    b1.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    // The fog's compute pass reads the map as well as the fragment shaders.
    VkPipelineStageFlags srcStage = (passLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
        : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    b1.srcStageMask = srcStage;
    VkDependencyInfo b1Dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b1Dep.dependencyFlags = 0;
    b1Dep.imageMemoryBarrierCount = 1;
    b1Dep.pImageMemoryBarriers = &b1;
    cmdPipelineBarrier2(currentCmd, b1Dep);

    // Begin the shadow pass, one way or the other.
    //
    // The first pass converted to dynamic rendering, and the one with least
    // to lose by it: one attachment, no colour, no resolve, a single
    // begin/end, and pipelines nothing else shares. Its render pass declared
    // an EXTERNAL->0 dependency covering the same fragment-read to
    // depth-write hazard that barrier 1 above already covers explicitly, so
    // nothing is lost by dropping the implicit half - the layout it wants is
    // the layout b1 leaves it in.
    const bool dynamicRendering = vkCtx->useDynamicRendering();
    // Said once, at warning level, because a bug report arrives with a
    // warnings-only log and "are the shadows drawn the new way" is the first
    // question this change makes anyone ask. A line here answers it without
    // a second run.
    static bool saidWhichPath = false;
    if (!saidWhichPath) {
        saidWhichPath = true;
        LOG_WARNING("Shadow pass records with ",
                    dynamicRendering ? "vkCmdBeginRendering" : "a VkRenderPass");
    }
    if (dynamicRendering) {
        VkRenderingAttachmentInfo depthAttach{};
        depthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttach.imageView = passView;
        depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttach.clearValue.depthStencil = {.depth = 1.0f, .stencil = 0};

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = mapSize, .height = mapSize}};
        renderInfo.layerCount = 1;
        renderInfo.pDepthAttachment = &depthAttach;
        vkCmdBeginRendering(currentCmd, &renderInfo);
    } else {
        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass = shadowRenderPass;
        rpInfo.framebuffer = passFramebuffer;
        rpInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = mapSize, .height = mapSize}};
        VkClearValue clear{};
        clear.depthStencil = {.depth = 1.0f, .stencil = 0};
        rpInfo.clearValueCount = 1;
        rpInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(currentCmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
    }

    VkViewport vp{.x = 0, .y = 0, .width = static_cast<float>(mapSize), .height = static_cast<float>(mapSize), .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(currentCmd, 0, 1, &vp);
    VkRect2D sc{.offset = {.x = 0, .y = 0}, .extent = {.width = mapSize, .height = mapSize}};
    vkCmdSetScissor(currentCmd, 0, 1, &sc);

    // Phase 7/8: render shadow casters
    // The units' map takes whatever reaches into its square: out to its
    // corners, and a few yards past them for a caster standing outside whose
    // body leans in.
    const float shadowCullRadius = strike ? 550.0f : contact ? CONTACT_MAP_HALF_EXTENT * 1.415f + 8.0f
                                           : frameInputs_.shadowDistance * 1.35f;
    if (drawCasters) {
    constexpr std::string_view kStageGroup = "beginFrame/graph/shadow_pass";
    if (terrainRenderer && !contact) {
        CpuStage stage(vkCtx->cpuStages(), kStageGroup, "sun: terrain");
        terrainRenderer->recordDepth(currentCmd, passMatrix, passCenter, shadowCullRadius, frameInputs_.scene->terrain);
    }
    if (wmoRenderer && !contact) {
        CpuStage stage(vkCtx->cpuStages(), kStageGroup, strike ? "lightning: wmo" : "sun: wmo");
        wmoRenderer->renderShadow(currentCmd, passMatrix, passCenter, shadowCullRadius, !wmoCasterSetsReset_);
        wmoCasterSetsReset_ = true;
    }
    if (m2Renderer) {
        CpuStage stage(vkCtx->cpuStages(), kStageGroup, strike ? "lightning: m2" : contact ? "units: m2" : "sun: m2");
        m2Renderer->renderShadow(currentCmd, passMatrix, frameInputs_.wind, passCenter, shadowCullRadius,
                                 !m2CasterSetsReset_, /*reuseCull=*/!contact && !strike);
        m2CasterSetsReset_ = true;
    }
    if (characterRenderer) {
        CpuStage stage(vkCtx->cpuStages(), kStageGroup, strike ? "lightning: characters" : contact ? "units: characters" : "sun: characters");
        characterRenderer->renderShadow(currentCmd, passMatrix, passCenter, shadowCullRadius, !characterCasterSetsReset_);
        characterCasterSetsReset_ = true;
    }
    }  // drawCasters

    if (dynamicRendering) {
        vkCmdEndRendering(currentCmd);
    } else {
        vkCmdEndRenderPass(currentCmd);
    }

    // Barrier 2: DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier2 b2{};
    b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b2.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    // Compute too: the volumetric fog samples it right after this pass.
    b2.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.image = passImage;
    b2.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    VkDependencyInfo b2Dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b2Dep.dependencyFlags = 0;
    b2Dep.imageMemoryBarrierCount = 1;
    b2Dep.pImageMemoryBarriers = &b2;
    cmdPipelineBarrier2(currentCmd, b2Dep);
    passLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkCtx) vkCtx->gpuMark(currentCmd, strike ? "lightning shadows" : contact ? "contact shadows" : "shadows");
}

VkImageView Renderer::getNeutralRtLightingView() const {
    return rtLighting_ ? rtLighting_->neutralView() : VK_NULL_HANDLE;
}

void Renderer::setRtLightingMode(int mode) {
    requestedRtMode_ = std::clamp(mode, 0, 3);
}

void Renderer::writeRtLightingBindings(uint32_t i) {
    if (!rtLighting_ || !vkCtx) return;
    // beginFrame has fenced this slot; the other slot can still be in use.
    {
        VkDescriptorImageInfo info[4]{};
        info[0].imageView = rtLighting_->lightViewForSlot(i);
        info[1].imageView = rtLighting_->giViewForSlot(i);
        info[2].imageView = rtLighting_->reflectionViewForSlot(i);
        info[3].imageView = rtLighting_->geometryViewForSlot(i);
        for (auto& image : info) image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t b = 0; b < 4; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = perFrameDescSets[i];
            writes[b].dstBinding = b < 2 ? 3 + b : 4 + b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &info[b];
        }
        vkUpdateDescriptorSets(vkCtx->getDevice(), 4, writes, 0, nullptr);
    }
}

VkExtent2D Renderer::sceneRenderExtent() const {
    if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
        return postProcessPipeline_->getSceneRenderExtent();
    }
    return vkCtx->getSwapchainExtent();
}

void Renderer::recordRtLighting(VkImage sceneDepth, VkExtent2D sceneExtent, bool depthIsMsaa,
                                std::string_view stageGroup) {
    if (!rtLighting_ || !rtLighting_->active() || !frameCamera()) return;
    rtRecordedThisFrame_ = true;
    RtLighting::FrameInputs in{};
    in.view = frameCamera()->getViewMatrix();
    in.projection = frameCamera()->getProjectionMatrix();
    in.cameraPos = frameCamera()->getPosition();
    in.sunDir = -glm::vec3(currentFrameData.lightDir);
    in.sunColor = glm::vec3(currentFrameData.lightColor);
    in.skyColor = glm::vec3(currentFrameData.ambientColor);
    in.sunUp = in.sunDir.z > -0.05f;
    in.distance = frameInputs_.rtDistance;
    GpuPass pass(*vkCtx, currentCmd, "rt_lighting");
    CpuStage stage(vkCtx->cpuStages(), stageGroup, "rt lighting");
    rtLighting_->record(currentCmd, sceneDepth, sceneExtent, depthIsMsaa, in);
}

VkImageView Renderer::getNeutralFogVolumeView() const {
    return volumetricFog_ ? volumetricFog_->getNeutralView() : VK_NULL_HANDLE;
}

void Renderer::setVolumetricFogQuality(int quality) {
    requestedFogQuality_ = std::clamp(quality, 0, 3);
}

void Renderer::writeFogVolumeBindings(uint32_t i) {
    if (!volumetricFog_ || !vkCtx) return;
    // beginFrame has fenced this slot; the other slot can still be in use.
    {
        VkDescriptorImageInfo fogImgInfo{};
        fogImgInfo.imageView = volumetricFog_->getVolumeView(i);
        fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = perFrameDescSets[i];
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &fogImgInfo;
        vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
    }
}

float Renderer::volumetricFogExtinction() const {
    // Per yard, at and below the layer, near the camera. At this a quarter of
    // the light is gone over the first thirty yards along the ground and two
    // thirds over a hundred, before the zone and the slider scale it: the
    // houses across a street stand in the mist, the trees past them are
    // shapes in it. A fifth of this, with the layer thirty yards deep, took a
    // sixth over a hundred and read as the picture dimmed, not as mist.
    constexpr float kBaseExtinction = 0.01f;
    float extinction = kBaseExtinction * volumetricFogDensity_;

    if (lightingManager) {
        const auto& lp = lightingManager->getLightingParams();
        // The zone's own opinion, read off how close its authored fog comes
        // in: Duskwood's ends at 525 yards and gets most of the mist, an
        // open plain's ends far out and gets less. The player's fog slider
        // divides those distances, so it is multiplied back out to reach the
        // zone's number rather than the slider's.
        //
        // The weather comes in here too, and only here: the light blends
        // toward the zone's storm row as it rains or snows, and that row's
        // fog is the zone's word on how close the weather brings it. A rain
        // and snow factor of our own on top - up to three times in a storm -
        // closed a jungle in to a wall in the first shower.
        const float strength = lightingManager->getFogStrength();
        if (strength > 0.001f && lp.fogEnd > 1.0f) {
            const float authoredEnd = lp.fogEnd * strength;
            extinction *= glm::clamp(900.0f / authoredEnd, 0.6f, 2.0f);
        }
        // Morning mist: thickest around half past six, gone by nine.
        const float hours = lightingManager->getTimeOfDayHours();
        const float dawn = 1.0f - glm::smoothstep(0.0f, 2.5f, std::abs(hours - 6.5f));
        extinction *= 1.0f + 0.8f * dawn;
    }

    // Indoors the outdoor air mostly stays outside. Not all of it: a hall
    // with torches in it should still show their glow.
    if (viewerMotion_.insideInterior) extinction *= 0.35f;
    return extinction;
}

void Renderer::renderSeaBedPass() {
    ZoneScopedN("Renderer::renderSeaBedPass");
    if (!seaBed_ || !waterRenderer || !frameCamera() || currentCmd == VK_NULL_HANDLE) return;
    // What the last drawing cost, once its timestamps are back. The frame
    // profile's own report is of one frame every ten seconds, and the map is
    // drawn in one frame of a few hundred.
    static const bool frameProfile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
    if (frameProfile && vkCtx) {
        for (const auto& [label, ms] : vkCtx->gpuTimings()) {
            if (label && std::strcmp(label, "sea bed") == 0) {
                LOG_WARNING("Sea bed map drawn in ", ms, "ms of GPU time");
            }
        }
    }
    // WOWEE_NO_SEA_BED=1: the water as if it were deep everywhere, waves to
    // the beach and through the piers, to compare against.
    static const bool noSeaBed = core::envFlagEnabled("WOWEE_NO_SEA_BED", false);
    if (noSeaBed) {
        static bool said = false;
        if (!said) {
            said = true;
            LOG_WARNING("Sea bed map off (WOWEE_NO_SEA_BED) - the waves keep their height to every shore");
        }
        return;
    }
    // Round the camera, which is where the waves are seen.
    const glm::vec2 nearby(frameCamera()->getPosition());
    const std::optional<float> water =
        frameInputs_.seaWaterHeight;
    if (!water) return;
    // The wind the fetch is found for: the way it blows, not its gusts.
    const glm::vec2 windToward = frameInputs_.windDirection;
    if (!seaBed_->wantsDrawing(nearby, *water, frameInputs_.time)) {
        // The same bed, a wind that has turned: the shore upwind is another.
        if (seaBed_->wantsFetch(windToward)) {
            seaBed_->refetch(currentCmd, windToward);
            if (vkCtx) vkCtx->gpuMark(currentCmd, "sea fetch");
        }
        return;
    }

    seaBed_->begin(nearby, *water);
    const OverheadFrame& frame = seaBed_->drawing();
    // The casters cull by the matrix's footprint and by distance; the map's
    // corners are its half extent and a half again from its middle.
    const glm::vec3 centre(frame.centre, frame.ceiling);
    const float reach = SeaBedMap::kHalfExtent * 1.415f;
    // The ground, and the buildings from above and from below: what the
    // water stands on and against. Not the doodads (SeaBedMap).
    const auto drawView = [&](SeaBedView view) {
        const glm::mat4 matrix = seaBed_->viewMatrix(view);
        seaBed_->beginView(currentCmd, view);
        if (view == SeaBedView::GroundFromAbove) {
            if (terrainRenderer) terrainRenderer->recordDepth(currentCmd, matrix, centre, frame.range, frameInputs_.scene->terrain);
        } else if (wmoRenderer) {
            // The sun's pass reset the casters' texture sets this frame if
            // it drew them; if it did not, this is the first, and resets.
            wmoRenderer->renderShadow(currentCmd, matrix, centre, reach, !wmoCasterSetsReset_);
            wmoCasterSetsReset_ = true;
        }
        seaBed_->endView(currentCmd, view);
    };
    drawView(SeaBedView::GroundFromAbove);
    drawView(SeaBedView::BuildingsFromAbove);
    drawView(SeaBedView::BuildingsFromBelow);
    seaBed_->finish(currentCmd, frameInputs_.time, windToward);
    if (vkCtx) vkCtx->gpuMark(currentCmd, "sea bed");
}

const LightingParams* Renderer::weatherLight() const {
    if (weatherLight_) return weatherLight_.get();
    return lightingManager ? &lightingManager->getLightingParams() : nullptr;
}

void Renderer::hazeLightForWeather() {
    if (!lightingManager) return;
    if (!weatherLight_) weatherLight_ = std::make_unique<LightingParams>();
    LightingParams p = lightingManager->getLightingParams();
    // Rain and snow bring no haze of their own: the light blends toward the
    // zone's storm row by them, and that row's fog says how close they bring
    // it. Blowing sand is the colour of sand in the day's light; the fog
    // weather's mist is grey.
    const WeatherMix& w = weather_.mix();
    if (w.sand > 0.0f || w.mist > 0.0f) {
        const glm::vec3 lit = p.ambientColor + p.diffuseColor * 0.5f;
        const glm::vec3 sandHaze = glm::vec3(0.80f, 0.64f, 0.42f) * lit;
        const glm::vec3 mistHaze = glm::mix(p.fogColor, glm::vec3(glm::dot(lit, glm::vec3(1.0f / 3.0f))), 0.5f);
        const auto haze = [&](const glm::vec3& c, float sandShare, float mistShare) {
            return glm::mix(glm::mix(c, sandHaze, w.sand * sandShare), mistHaze, w.mist * mistShare);
        };
        p.fogColor = haze(p.fogColor, 0.85f, 0.6f);
        // The sky hazes too, the horizon most, so the hills are not cut out
        // against a clear sky behind the dust.
        p.skyTopColor = haze(p.skyTopColor, 0.6f, 0.4f);
        p.skyMiddleColor = haze(p.skyMiddleColor, 0.8f, 0.5f);
        p.skyBand1Color = haze(p.skyBand1Color, 0.85f, 0.6f);
        p.skyBand2Color = haze(p.skyBand2Color, 0.85f, 0.6f);
        p.skySmogColor = haze(p.skySmogColor, 0.85f, 0.6f);
        p.cloudFillColor = haze(p.cloudFillColor, 0.6f, 0.4f);
        p.cloudBaseColor = haze(p.cloudBaseColor, 0.6f, 0.4f);
        p.fogStart = glm::mix(glm::mix(p.fogStart, 2.0f, w.sand), 10.0f, w.mist * 0.8f);
        p.fogEnd = glm::mix(glm::mix(p.fogEnd, 140.0f, w.sand), 220.0f, w.mist * 0.8f);
    }
    *weatherLight_ = p;
}

void Renderer::setWeatherDetail(float scale) {
    weatherDetail_ = std::clamp(scale, 0.0f, 1.0f);
    if (precipitation_) precipitation_->setDensityScale(weatherDetail_);
}

float Renderer::skyExposure() const {
    return publishedExposure_;
}

float Renderer::skyView() const {
    return publishedSkyView_;
}

uint32_t Renderer::precipitationCount() const {
    return precipitation_ ? precipitation_->drawnCount() : 0;
}

void Renderer::updateWeather(float deltaTime) {
    ZoneScopedN("Renderer::updateWeather");
    if (!camera) return;
    const WeatherMix& mix = weather_.mix();
    const glm::vec3 eye = camera->getPosition();
    const WindState& wind = wind_.state();
    const glm::vec2 windVelocity = wind.direction * wind.speed * wind.gust;

    if (precipitation_) {
        PrecipitationFrame frame;
        if (weatherControls_.precipitation) frame.mix = mix;
        frame.camera = eye;
        frame.wind = windVelocity;
        frame.dt = deltaTime;
        frame.water = &liquidScene_;
        frame.now = globalTime;
        frame.openSky = skyView();
        frame.viewport = {publishedStatistics_.width, publishedStatistics_.height};
        if (!frame.viewport.width && surface_) frame.viewport = {static_cast<uint32_t>(std::max(0, surface_->getDrawableWidth())), static_cast<uint32_t>(std::max(0, surface_->getDrawableHeight()))};
        precipitation_->setSplashesEnabled(weatherControls_.splashes);
        precipitation_->update(frame);
    }

    // The lens: how squarely it faces into what falls - rain falls along the
    // wind, at its own speed downward - and into the wind for the sand.
    const glm::vec3 forward = camera->getForward();
    const glm::vec3 fall = glm::normalize(glm::vec3(windVelocity * 0.9f, -10.0f));
    const float windSpeed = glm::length(windVelocity);
    const glm::vec3 windDir = windSpeed > 0.1f ? glm::vec3(windVelocity / windSpeed, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    LensInputs lens;
    if (weatherControls_.lens) {
        // Fewer reach a camera walled in, as fewer fall in its view.
        const float walledIn = glm::mix(0.5f, 1.0f, skyView());
        lens.rain = mix.rain * walledIn;
        lens.snow = mix.snow * walledIn;
        lens.sand = mix.sand * walledIn;
    }
    lens.exposure = skyExposure();
    lens.facingFall = glm::smoothstep(-0.25f, 0.6f, glm::dot(forward, -fall));
    lens.facingWind = glm::smoothstep(-0.2f, 0.7f, glm::dot(forward, -windDir));
    // Across the screen as the wind crosses the view.
    const glm::vec2 across(glm::dot(windDir, camera->getRight()), -glm::dot(windDir, camera->getUp()));
    lensWindOnScreen_ = across * std::clamp(std::max(windSpeed, 9.0f) * 0.15f, 0.0f, 3.0f);
    lens.windOnScreen = lensWindOnScreen_;
    const auto eyeDepth = cameraWaterDepth();
    lens.underwater = eyeDepth && *eyeDepth > 0.0f;
    if (!weatherControls_.lens) lensDrops_.clear();
    lensDrops_.update(deltaTime, lens, camera->getAspectRatio());

    // Lightning over a thunderstorm, as often as it is heavy.
    if (lightning) {
        const bool storm = weatherControls_.lightning && mix.thunder > 0.05f;
        if (lightningMapId_ != environment_.mapId) {
            lightning->reset();
            lightningMapId_ = environment_.mapId;
        }
        lightning->setIntensity(mix.thunder);
        lightning->setEnabled(storm);
        lightning->update(deltaTime, *camera, [this](const glm::vec3& point) -> std::optional<float> {
            std::optional<float> height;
            height = collision_->terrainHeight(point.x, point.y);
            {
                const glm::vec3 origin = point + glm::vec3(0, 0, 500);
                const float hit = collision_->wmoRaycast(collision::Reach::everywhere(), origin,
                    glm::vec3(0, 0, -1), 1500.0f, /*cameraQuery=*/false);
                if (hit < 1500.0f && (!height || origin.z - hit > *height)) height = origin.z - hit;
            }
            if (!height) LOG_WARNING("Lightning at ", point.x, ",", point.y,
                                     " has no loaded terrain or building; strike skipped");
            return height;
        });
    }
}

void Renderer::renderShelterPass() {
    ZoneScopedN("Renderer::renderShelterPass");
    if (!shelter_ || !frameCamera() || currentCmd == VK_NULL_HANDLE) return;
    const glm::vec3 eye = frameCamera()->getPosition();
    // What is loaded, as a number that moves when any of it streams in or out.
    const uint64_t world = frameInputs_.shelterRevision;
    if (shelter_->wantsDrawing(eye, frameInputs_.time, world)) {
        shelter_->begin(currentCmd, eye, world);
        const glm::mat4 matrix = shelter_->matrix();
        const glm::vec3 centre(shelter_->drawing().centre, eye.z);
        // The map's corners are its half extent and a half again from its
        // middle.
        const float reach = ShelterMap::kHalfExtent * 1.415f;
        // The ground, the buildings and the doodads - a tree's crown keeps
        // the rain off as a roof does. Not the characters.
        if (terrainRenderer) terrainRenderer->recordDepth(currentCmd, matrix, centre, reach, frameInputs_.scene->terrain);
        if (wmoRenderer) {
            wmoRenderer->renderShadow(currentCmd, matrix, centre, reach, !wmoCasterSetsReset_);
            wmoCasterSetsReset_ = true;
        }
        if (m2Renderer) {
            m2Renderer->renderShadow(currentCmd, matrix, frameInputs_.wind, centre, reach, !m2CasterSetsReset_);
            m2CasterSetsReset_ = true;
        }
        shelter_->end(currentCmd, frameInputs_.time);
        if (vkCtx) vkCtx->gpuMark(currentCmd, "shelter");
    }
    shelter_->probe(currentCmd, vkCtx->getCurrentFrame(), eye);
}

void Renderer::recordLensWeather() {
    if (!lensWeather_ || currentCmd == VK_NULL_HANDLE) return;
    const auto& images = vkCtx->getSwapchainImages();
    // The world's picture only: a loading screen is not seen through the lens.
    const bool onScreen = worldDrawnThisFrame_ && currentImageIndex < images.size();
    lensWeather_->record(currentCmd, vkCtx->getCurrentFrame(), onScreen ? images[currentImageIndex] : VK_NULL_HANDLE,
                         vkCtx->getSwapchainExtent(), frameInputs_.lensDrops, frameInputs_.lensWind,
                         std::fmod(frameInputs_.time, 1000.0f));
}

void Renderer::renderVolumetricFog() {
    ZoneScopedN("Renderer::renderVolumetricFog");
    if (!volumetricThisFrame_ || !volumetricFog_ || !frameCamera() || currentCmd == VK_NULL_HANDLE) return;
    const uint32_t frame = vkCtx->getCurrentFrame();
    // The inject pass samples this slot's shadow map, so not before the shadow
    // pass has left it readable: at the login screen, before the player has a
    // position, it has never been drawn and is still UNDEFINED, which the
    // validation layer reports for any dispatch that binds it. Skipping leaves
    // this slot's volume as it was - clear air, until the first real frame -
    // and the shaders read that.
    if (shadowDepthLayout_[frame] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    const float dt = std::max(frameInputs_.deltaTime, 0.0f);

    // The ground the mist lies on: the terrain under the player, or the
    // player's own feet where those are lower - a cave, a city under a
    // mountain - so the layer is never a ceiling over them. Chased over a
    // second or two so a step off a ledge does not lift the whole bank of
    // mist with it; a teleport snaps it, and drops last frame's air too.
    const float ground = frameInputs_.fogGround;
    if (!fogLayerBaseValid_ || std::abs(ground - fogLayerBase_) > 150.0f) {
        fogLayerBase_ = ground;
        fogLayerBaseValid_ = true;
        volumetricFog_->resetHistory();
    } else {
        fogLayerBase_ += (ground - fogLayerBase_) * (1.0f - std::exp(-dt / 1.5f));
    }

    const bool strikeLight = currentFrameData.lightningColor.b > 0.0f;
    if (strikeLight || lightningFogLastFrame_) volumetricFog_->resetHistory();
    lightningFogLastFrame_ = strikeLight;
    const float target = frameInputs_.fogTargetExtinction;
    if (fogExtinction_ < 0.0f) fogExtinction_ = target;
    fogExtinction_ += (target - fogExtinction_) * (1.0f - std::exp(-dt / 1.0f));

    VolumetricFog::FrameInputs in;
    in.view = currentFrameData.view;
    in.projection = currentFrameData.projection;
    in.cameraPos = glm::vec3(currentFrameData.viewPos);
    in.time = frameInputs_.time;
    in.density = fogExtinction_;
    in.layerBase = fogLayerBase_;
    if (frameInputs_.hasLighting) {
        // The air's forward glow in the light tables' sunlit-cloud colour.
        in.sunHaloColor = frameInputs_.lighting.cloudSunlitColor;
        in.sunGlareDirection =
            sky_celestial::sunDisc(frameInputs_.timeOfDay / 24.0f).direction;
        in.sunGlareAmount = frameInputs_.sunGlareSky * sky_celestial::weatherFade(frameInputs_.weather.strength());
    }
    volumetricFog_->record(currentCmd, frame, perFrameDescSets[frame], in);
    if (vkCtx) vkCtx->gpuMark(currentCmd, "volumetric fog");

    // What it was built from, every few seconds, for the report that says the
    // fog is too thick or missing: INFO, so it costs nothing unless asked for.
    static double lastFogLog = 0.0;
    if (frameInputs_.time - lastFogLog > 5.0) {
        lastFogLog = frameInputs_.time;
        LOG_INFO("volumetricFog: extinction=", fogExtinction_, "/yd (target ", target,
                 ") layerBase=", fogLayerBase_, " lights=", currentFrameData.localLightMeta.x);
    }
}

void Renderer::recordSunShafts() {
    if (!sunShafts_ || currentCmd == VK_NULL_HANDLE) return;
    SunShafts::FrameInputs in;
    const auto& images = vkCtx->getSwapchainImages();
    if (frameInputs_.sunShafts && worldDrawnThisFrame_ && frameCamera() && frameInputs_.hasLighting &&
        currentImageIndex < images.size() &&
        !(passAblation_ && passAblation_->skip(AblationPass::SunShafts))) {
        const auto& lp = frameInputs_.lighting;
        // Around the disc, where the glare is - not the light's direction,
        // which never sets.
        const glm::vec3 sunDir =
            sky_celestial::sunDisc(frameInputs_.timeOfDay / 24.0f).direction;
        const SunOnScreen sun = sunScreenPosition(frameCamera()->getViewMatrix(),
                                                  frameCamera()->getProjectionMatrix(), sunDir);
        if (sun.inFront) {
            // Screen strength: 1 would be the sky's own colour over anything a
            // fully lit walk crosses. Less, because the sky around the sun is
            // already the brightest thing on screen.
            float strength = 0.8f;
            // As much as the sky round the sun shines: the glare's hours, the
            // cloud in front, a sky model over it and the weather. Not its line
            // of sight - the rays are what a tree in front lets through.
            strength *= frameInputs_.sunGlareSky * sky_celestial::weatherFade(frameInputs_.weather.strength());
            // In view, or streaming in from just past an edge, fading out as
            // it goes half a screen beyond one.
            const glm::vec2 past = glm::max(glm::abs(sun.uv - 0.5f) - 0.5f, glm::vec2(0.0f));
            strength *= 1.0f - glm::smoothstep(0.0f, 0.5f, std::max(past.x, past.y));

            // The rays are the eye looking into the sun, so they take the
            // sun's own colour from LightParams - pale green over Tirisfal,
            // blue over Duskwood. They were the light it casts, normalized
            // and mixed half with white, the same near-white everywhere.
            in.tint = glm::min(lp.sunColor, glm::vec3(1.0f));
            in.sunUV = sun.uv;
            in.strength = strength;
        }
    }
    // Called every frame, strength zero included, so the composite knows
    // there is nothing of this frame's to add.
    sunShafts_->record(currentCmd, vkCtx->getCurrentFrame(),
                       currentImageIndex < images.size() ? images[currentImageIndex] : VK_NULL_HANDLE,
                       vkCtx->getSwapchainExtent(), in);
}

// Build the per-frame render graph for off-screen pre-passes.
// Declares passes as graph nodes with input/output dependencies.
// compile() performs topological sort; execute() runs them with auto barriers.
void Renderer::buildFrameGraph() {
    if (!renderGraph_) return;

    renderGraph_->reset();
    characterCasterSetsReset_ = wmoCasterSetsReset_ = m2CasterSetsReset_ = false;

    auto shadowDepth = renderGraph_->findResource("shadow_depth");
    auto reflTex = renderGraph_->findResource("reflection_texture");

    // Minimap composites (no dependencies - standalone off-screen render target)
    renderGraph_->addPass("minimap_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            if (minimap && frameInputs_.scene) minimap->recordComposite(cmd, frameInputs_.scene->minimap);
        });

    // What the interface shows from images of its own: the world map, the
    // character previews (standalone).
    renderGraph_->addPass("offscreen_composites", {}, {},
        [this](VkCommandBuffer cmd) {
            const uint32_t frame = vkCtx->getCurrentFrame();
            for (const auto& request : frameInputs_.scene->composites) request(cmd, frame);
        });

    // The far ground's bake: new tiles' atlases readied and their chunks
    // drawn into them, before anything draws the ground this frame.
    renderGraph_->addPass("terrain_bake", {}, {},
        [this](VkCommandBuffer cmd) {
            if (terrainRenderer) terrainRenderer->recordBake(cmd, frameInputs_.scene->terrain);
        });

    // Shadow pre-pass → outputs shadow_depth
    renderGraph_->addPass("shadow_pass", {}, {shadowDepth},
        [this](VkCommandBuffer) {
            // Not gated on shadowsEnabled: renderShadowPass is what clears the
            // map and leaves it in the layout its readers expect, and it
            // already skips the casters on its own when shadows are off.
            // Declining to call it here put the transition back where it was
            // before, which is the whole of the fault this was meant to end.
            if (shadowDepthImage[0] != VK_NULL_HANDLE)
                renderShadowPass();
        });
    // Left enabled even with shadows off, as long as the image exists.
    //
    // A disabled pass is skipped whole, and that includes the image barriers
    // declared on it - so turning shadows off stopped the shadow map ever
    // being transitioned, while the passes that read it kept it bound and
    // sampled it in whatever layout it was last left in. The lambda above
    // already declines to draw anything; what has to keep happening is the
    // transition.
    renderGraph_->setPassEnabled("shadow_pass", shadowDepthImage[0] != VK_NULL_HANDLE);

    // Volumetric fog → reads this frame's shadow map, outputs the fog volume
    // every world shader samples.
    auto fogVolume = renderGraph_->findResource("volumetric_fog");
    renderGraph_->addPass("volumetric_fog", {shadowDepth}, {fogVolume},
        [this](VkCommandBuffer) {
            renderVolumetricFog();
        });
    renderGraph_->setPassEnabled("volumetric_fog", volumetricThisFrame_);

    // What lies under the water → read by the water for its depth. After
    // the shadow pass, whose casters' texture sets it reuses.
    auto seaBedRes = renderGraph_->findResource("sea_bed");
    renderGraph_->addPass("sea_bed", {shadowDepth}, {seaBedRes},
        [this](VkCommandBuffer) {
            renderSeaBedPass();
        });
    renderGraph_->setPassEnabled("sea_bed", seaBed_ && waterRenderer && frameInputs_.scene->water.inView &&
                                                !waterAblated());

    // Where rain and snow cannot reach → read by the precipitation. After
    // the shadow pass too, for the same casters' texture sets. Only while
    // something falls or blows.
    auto shelterRes = renderGraph_->findResource("shelter");
    renderGraph_->addPass("shelter", {shadowDepth}, {shelterRes},
        [this](VkCommandBuffer) {
            renderShelterPass();
        });
    renderGraph_->setPassEnabled("shelter", shelter_ && frameInputs_.weather.any());

    // The sea → outputs the surface every water surface is carried by and
    // lit from. Only while there is water in view; the sea's clock and its
    // wind go on regardless, and it catches up when water comes back.
    auto oceanSurface = renderGraph_->findResource("ocean_surface");
    renderGraph_->addPass("ocean_waves", {}, {oceanSurface},
        [this](VkCommandBuffer) {
            if (waterRenderer && currentCmd != VK_NULL_HANDLE) {
                waterRenderer->recordWaves(currentCmd, frameInputs_.scene->water);
                if (vkCtx) vkCtx->gpuMark(currentCmd, "ocean waves");
            }
        });
    renderGraph_->setPassEnabled("ocean_waves", waterRenderer && frameInputs_.scene->water.inView &&
                                                    !waterAblated());

    // Reflection pre-pass → outputs reflection_texture (reads scene, so after shadow)
    renderGraph_->addPass("reflection_pass", {shadowDepth}, {reflTex},
        [this](VkCommandBuffer) {
            renderReflectionPass();
        });

    renderGraph_->compile();
}

} // namespace rendering
} // namespace wowee

namespace wowee::rendering {
std::unique_ptr<TerrainManager> Renderer::takeTerrainSession() {
    if (!standaloneTerrain_ || terrainManager != standaloneTerrain_.get())
        throw std::logic_error("Terrain session already has a scene owner");
    return std::move(standaloneTerrain_);
}
void Renderer::releaseTerrainSession(TerrainManager* session) {
    if (terrainManager != session) {
        LOG_ERROR("Terrain session released by a different scene owner");
        return;
    }
    terrainManager = nullptr;
}
}
