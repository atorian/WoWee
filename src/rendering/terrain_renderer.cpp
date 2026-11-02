#include "rendering/terrain_vertex.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/texture_upload.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/cpu_stage_times.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/frustum.hpp"
#include "rendering/water_reflection.hpp"
#include "rendering/terrain_fog_band.hpp"
#include "rendering/normal_map.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/env_flag.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <bit>
#include <functional>
#include <cstdlib>
#include <unordered_set>
#include <limits>
#include <cstring>

namespace wowee {
namespace rendering {


/// Chunks drawn into their atlases a frame. A tile is 256; one arriving bakes
/// in the frame after it, and the world's first hundred tiles in seventy.
constexpr size_t kBakeChunksPerFrame = 512;

/// The Sobel strength the ground's relief is derived with, half a wall's. The
/// ground is seen at a grazing angle across a whole road at once, where the
/// WMO renderer's two turned every grain of a cobble into a chiselled edge;
/// at one, Goldshire's road reads as stones and mortar at the default "Bump
/// strength", which scales the result in the shader.
constexpr float kTerrainNormalMapStrength = 1.0f;

namespace {

/// Every layer of a tile's chunk maps from tightly packed RGBA8 pixels, layer
/// after layer, in one submission: into the open upload batch where there is
/// one, else waited for. Both images go from undefined to shader-readable.
bool uploadTileMapLayers(VkContext& ctx, TerrainTileMaps& maps, const uint8_t* cover,
                         const uint8_t* normals, uint32_t coverSize, uint32_t normalSize,
                         uint32_t layers) {
    const VkDeviceSize coverBytes = VkDeviceSize{coverSize} * coverSize * 4 * layers;
    const VkDeviceSize normalBytes = VkDeviceSize{normalSize} * normalSize * 4 * layers;
    AllocatedBuffer staging = createBuffer(ctx.getAllocator(), coverBytes + normalBytes,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    if (!staging.buffer) return false;
    void* mapped = nullptr;
    if (vmaMapMemory(ctx.getAllocator(), staging.allocation, &mapped) != VK_SUCCESS) {
        destroyBuffer(ctx.getAllocator(), staging);
        return false;
    }
    std::memcpy(mapped, cover, coverBytes);
    std::memcpy(static_cast<uint8_t*>(mapped) + coverBytes, normals, normalBytes);
    vmaUnmapMemory(ctx.getAllocator(), staging.allocation);

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                                 VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
            VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                  .levelCount = 1, .baseArrayLayer = 0, .layerCount = layers};
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        const auto copy = [&](VkImage image, VkDeviceSize offset, uint32_t size) {
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                                       .baseArrayLayer = 0, .layerCount = layers};
            region.imageExtent = {.width = size, .height = size, .depth = 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        };
        for (VkImage image : {maps.cover.image, maps.normals.image}) {
            barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        copy(maps.cover.image, 0, coverSize);
        copy(maps.normals.image, coverBytes, normalSize);
        for (VkImage image : {maps.cover.image, maps.normals.image}) {
            barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    });

    if (ctx.isInUploadBatch()) {
        ctx.deferStagingCleanup(staging);
    } else {
        destroyBuffer(ctx.getAllocator(), staging);
    }
    return true;
}

void destroyTileMaps(VkDevice device, VmaAllocator allocator, TextureTable& table, TerrainTileMaps& maps) {
    table.remove(std::exchange(maps.coverHandle, kNoTextureHandle), TextureTable::Kind::Layered);
    table.remove(std::exchange(maps.normalsHandle, kNoTextureHandle), TextureTable::Kind::Layered);
    destroyImage(device, allocator, maps.cover);
    destroyImage(device, allocator, maps.normals);
}

/// The maps' places in the texture table, sampled through `sampler`. False,
/// with nothing taken, when the table had no room (it said so).
bool placeTileMaps(TextureTable& table, VkSampler sampler, TerrainTileMaps& maps) {
    maps.coverHandle = table.add(maps.cover.imageView, sampler, TextureTable::Kind::Layered);
    maps.normalsHandle = table.add(maps.normals.imageView, sampler, TextureTable::Kind::Layered);
    if (maps.coverHandle != kNoTextureHandle && maps.normalsHandle != kNoTextureHandle) return true;
    table.remove(std::exchange(maps.coverHandle, kNoTextureHandle), TextureTable::Kind::Layered);
    table.remove(std::exchange(maps.normalsHandle, kNoTextureHandle), TextureTable::Kind::Layered);
    return false;
}

}  // namespace

TerrainRenderer::TerrainRenderer() = default;

TerrainRenderer::~TerrainRenderer() {
    shutdown();
}

/// Builds the fill pipeline, its twin without the distance discards and its
/// wireframe derivative from a loaded shader pair.
///
/// initialize() and recreatePipelines() both need exactly these three in
/// exactly these states, and each described them for itself. Answers whether the fill
/// pipeline built; the wireframe is a debug view, so its absence is a warning
/// rather than a failure. Destroying the shader modules is left to the caller,
/// which loaded them.
bool TerrainRenderer::buildMainPassPipelines(VkDevice device,
                                             wowee::rendering::VkShaderModule& vertShader,
                                             wowee::rendering::VkShaderModule& fragShader) {
    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding = perVertexBinding(sizeof(pipeline::TerrainVertex));
    const std::vector<VkVertexInputAttributeDescription> vertexAttribs =
        toVkAttributes(kTerrainVertexAttributes);

    // --- Build fill pipeline (base for derivatives - shared state optimization) ---
    VkRenderPass mainPass = vkCtx->getImGuiRenderPass();

    pipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)
        .build(device, vkCtx->getPipelineCache());

    if (!pipeline) {
        LOG_ERROR("TerrainRenderer: failed to create fill pipeline");
        return false;
    }

    // --- Build the fill pipeline without the distance discards (derivative) ---
    const VkBool32 discardsOff = VK_FALSE;
    const VkSpecializationMapEntry discardsEntry{.constantID = 0, .offset = 0,
                                                 .size = sizeof(VkBool32)};
    const VkSpecializationInfo discardsSpec{.mapEntryCount = 1, .pMapEntries = &discardsEntry,
                                            .dataSize = sizeof(VkBool32), .pData = &discardsOff};
    VkPipelineShaderStageCreateInfo noDiscardFrag = fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);
    noDiscardFrag.pSpecializationInfo = &discardsSpec;
    shortOfDiscardsPipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), noDiscardFrag)
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(pipeline)
        .build(device, vkCtx->getPipelineCache());

    if (!shortOfDiscardsPipeline) {
        LOG_WARNING("TerrainRenderer: pipeline without the distance discards not available");
    }

    // --- Build wireframe pipeline (derivative of fill) ---
    // VK_POLYGON_MODE_LINE needs fillModeNonSolid, which plenty of mobile GPUs
    // do not have. Asking anyway is a validation error and a null pipeline;
    // drawing falls back to the filled one either way.
    if (!vkCtx->isWireframeSupported()) {
        LOG_WARNING("TerrainRenderer: wireframe views need fillModeNonSolid, "
                    "which this device does not support");
        return true;
    }

    wireframePipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(pipeline)
        .build(device, vkCtx->getPipelineCache());

    if (!wireframePipeline) {
        LOG_WARNING("TerrainRenderer: wireframe pipeline not available");
    }

    return true;
}

bool TerrainRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                  pipeline::AssetManager* assets) {
    vkCtx = ctx;
    assetManager = assets;

    if (!vkCtx || !assetManager) {
        LOG_ERROR("TerrainRenderer: null context or asset manager");
        return false;
    }

    LOG_INFO("Initializing terrain renderer (Vulkan)");
    VkDevice device = vkCtx->getDevice();

    lodEnabled_ = core::envFlagEnabled("WOWEE_TERRAIN_LOD", true);
    if (!lodEnabled_) {
        LOG_WARNING("WOWEE_TERRAIN_LOD=0: every terrain chunk is drawn at full detail, however far");
    }
    // What the cull would drop drawn anyway, to compare the view against
    // (check_terrain_cull.py).
    if (!core::envFlagEnabled("WOWEE_TERRAIN_FRUSTUM_CULL", true)) {
        frustumCullingEnabled = false;
        LOG_WARNING("WOWEE_TERRAIN_FRUSTUM_CULL=0: every terrain chunk in reach is drawn, in view or not");
    }

    // --- The chunk table's set, one per frame slot ---
    // The chunk rows, and the frame's draw records the cull writes.
    const VkDescriptorSetLayoutBinding chunkBindings[] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    chunkSetLayout_ = createDescriptorSetLayout(device, {chunkBindings[0], chunkBindings[1]});
    const VkDescriptorPoolSize chunkPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &chunkPoolSize;
    if (!chunkSetLayout_ || vkCreateDescriptorPool(device, &poolInfo, nullptr, &chunkSetPool_) != VK_SUCCESS) {
        LOG_ERROR("TerrainRenderer: could not make the chunk table's sets");
        return false;
    }
    for (auto& set : chunkSets_) {
        VkDescriptorSetAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = chunkSetPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &chunkSetLayout_;
        if (vkAllocateDescriptorSets(device, &allocInfo, &set) != VK_SUCCESS) {
            LOG_ERROR("TerrainRenderer: could not allocate the chunk table's sets");
            return false;
        }
    }
    chunkRows_.initialize(*vkCtx);
    cullRows_.initialize(*vkCtx);
    if (!createCull()) return false;
    const VkDescriptorSetLayout tableLayout = vkCtx->textureTable().layout();

    // The far ground's bake, which says itself why when it cannot be had.
    if (!core::envFlagEnabled("WOWEE_TERRAIN_BAKE", true)) {
        LOG_WARNING("WOWEE_TERRAIN_BAKE=0: every terrain chunk is drawn from its layers, however far");
    } else {
        bakeEnabled_ = bake_.initialize(*vkCtx, tableLayout, chunkSetLayout_);
    }

    // --- Create pipeline layout ---
    // The model matrix alone is pushed; everything of a chunk's draw is in
    // its record (terrain_cull.comp.glsl).
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, tableLayout, chunkSetLayout_ };
    pipelineLayout = createPipelineLayout(device, setLayouts, { pushRange });
    if (!pipelineLayout) {
        LOG_ERROR("TerrainRenderer: failed to create pipeline layout");
        return false;
    }

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/terrain.vert.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/terrain.frag.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load fragment shader");
        return false;
    }

    // --- Vertex input ---
    if (!buildMainPassPipelines(device, vertShader, fragShader)) {
        vertShader.destroy();
        fragShader.destroy();
        return false;
    }

    vertShader.destroy();
    fragShader.destroy();

    // --- Create fallback textures ---
    const uint8_t whitePixel[4] = {255, 255, 255, 255};
    whiteTexture = adoptPixels(*vkCtx, TextureForm::Owned, whitePixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                               false, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    // Straight up, for a texture whose relief could not be had.
    const uint8_t flatNormal[4] = {128, 128, 255, 128};
    flatNormalTexture = adoptPixels(*vkCtx, TextureForm::Owned, flatNormal, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                    false, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    // The tiles' chunk maps are sampled as each chunk's alpha map and normal
    // image were: linear, clamped to the chunk's edge, one level.
    VkSamplerCreateInfo mapSampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    mapSampler.magFilter = VK_FILTER_LINEAR;
    mapSampler.minFilter = VK_FILTER_LINEAR;
    mapSampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    mapSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    mapSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    mapSampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    mapSampler.anisotropyEnable = vkCtx->isSamplerAnisotropySupported() ? VK_TRUE : VK_FALSE;
    mapSampler.maxAnisotropy = 16.0f;
    mapSampler.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    mapSampler.maxLod = 0.0f;
    tileMapSampler_ = vkCtx->getOrCreateSampler(mapSampler);

    // One layer covering everything, lit flat, for a tile whose maps fail.
    {
        const uint8_t coverAll[4] = {255, 255, 255, 255};
        auto fallback = std::make_shared<TerrainTileMaps>();
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        fallback->cover = createArrayImage(device, vkCtx->getAllocator(), 1, 1, 1,
                                           VK_FORMAT_R8G8B8A8_UNORM, usage);
        fallback->normals = createArrayImage(device, vkCtx->getAllocator(), 1, 1, 1,
                                             VK_FORMAT_R8G8B8A8_UNORM, usage);
        if (!tileMapSampler_ || !fallback->cover.imageView || !fallback->normals.imageView ||
            !uploadTileMapLayers(*vkCtx, *fallback, coverAll, flatNormal, 1, 1, 1) ||
            !placeTileMaps(vkCtx->textureTable(), tileMapSampler_, *fallback)) {
            LOG_ERROR("TerrainRenderer: the fallback chunk maps could not be made, so no chunk can be drawn");
            destroyImage(device, vkCtx->getAllocator(), fallback->cover);
            destroyImage(device, vkCtx->getAllocator(), fallback->normals);
            return false;
        }
        fallbackTileMaps_ = std::move(fallback);
    }

    // The geometry pages are made as tiles arrive.
    geometry_.initialize(*vkCtx);

    LOG_INFO("Terrain renderer initialized (Vulkan)");
    return true;
}

void TerrainRenderer::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    // Destroy old pipelines (keep layouts)
    destroy(device, pipeline);
    destroy(device, shortOfDiscardsPipeline);
    destroy(device, wireframePipeline);

    // Load shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/terrain.vert.spv")) {
        LOG_ERROR("TerrainRenderer::recreatePipelines: failed to load vertex shader");
        return;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/terrain.frag.spv")) {
        LOG_ERROR("TerrainRenderer::recreatePipelines: failed to load fragment shader");
        vertShader.destroy();
        return;
    }

    buildMainPassPipelines(device, vertShader, fragShader);

    vertShader.destroy();
    fragShader.destroy();
}

void TerrainRenderer::shutdown() {
    LOG_INFO("Shutting down terrain renderer");

    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    vkCtx->waitIdle();

    clear();
    // clear() defers chunk destruction, and no further frames will run to drain
    // the queue - so run it now, while the descriptor pools those lambdas free
    // sets from are still alive. Without this every resident chunk's vertex and
    // index buffers outlived the device: twenty thousand of each.
    vkCtx->flushDeferredCleanup();
    // After the flush, which destroyed the atlases clear() let go; before the
    // chunk set layout its pipeline layout names.
    bake_.shutdown();
    bakeEnabled_ = false;

    loggedTextureLoadFails_.clear();
    loggedNormalMapFails_.clear();
    whiteTexture.reset();
    flatNormalTexture.reset();
    // After the flush, which destroyed the tiles' maps clear() let go. The
    // sampler is the context's cache's.
    if (fallbackTileMaps_) destroyTileMaps(device, allocator, vkCtx->textureTable(), *fallbackTileMaps_);
    fallbackTileMaps_.reset();
    tileMapSampler_ = VK_NULL_HANDLE;

    destroy(device, pipeline);
    destroy(device, shortOfDiscardsPipeline);
    destroy(device, wireframePipeline);
    destroy(device, pipelineLayout);
    destroyCull();
    chunkRows_.destroy();
    destroy(device, chunkSetPool_);
    destroy(device, chunkSetLayout_);
    for (auto& set : chunkSets_) set = VK_NULL_HANDLE;

    // Shadow pipeline cleanup
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroyShadowParamsSet(device, allocator, shadowParams_);

    // Every chunk's ranges came back in the flush above; the device is idle.
    geometry_.destroy();

    vkCtx = nullptr;
}

bool TerrainRenderer::loadTerrain(const pipeline::TerrainMesh& mesh,
                                   const std::vector<std::string>& texturePaths,
                                   int tileX, int tileY) {
    if (mesh.validChunkCount == 0) {
        LOG_WARNING("loadTerrain[", tileX, ",", tileY, "]: mesh has 0 valid chunks (", texturePaths.size(), " textures)");
        return false;
    }
    LOG_DEBUG("Loading terrain mesh: ", mesh.validChunkCount, " chunks");

    geometry_.releaseEmptyPages();
    vkCtx->beginUploadBatch();
    const std::shared_ptr<const TerrainTileMaps> maps = tileMapsFor(tileX, tileY, mesh);

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            const auto& chunk = mesh.getChunk(x, y);
            if (!chunk.isValid()) continue;

            TerrainChunkGPU gpuChunk = uploadChunk(chunk);
            if (!gpuChunk.isValid()) {
                LOG_WARNING("Failed to upload chunk [", x, ",", y, "]");
                continue;
            }
            gpuChunk.maps = {maps, TerrainChunkSlot::at(x, y)};

            calculateBoundingSphere(gpuChunk, chunk);

            // Load textures for this chunk
            bindChunkTextures(gpuChunk, chunk, texturePaths, tileX, tileY, x, y);

            gpuChunk.tileX = tileX;
            gpuChunk.tileY = tileY;

            if (!writeChunkRow(gpuChunk)) {
                destroyChunkGPU(gpuChunk);
                continue;
            }

            registerRtChunk(gpuChunk, chunk);
            addChunk(std::move(gpuChunk));
        }
    }

    vkCtx->endUploadBatch();

    LOG_DEBUG("Loaded ", chunks.size(), " terrain chunks to GPU");
    return !chunks.empty();
}

bool TerrainRenderer::loadTerrainIncremental(const pipeline::TerrainMesh& mesh,
                                              const std::vector<std::string>& texturePaths,
                                              int tileX, int tileY,
                                              int& chunkIndex, int maxChunksPerCall) {
    // Pages the tiles that left emptied, now that their frames are done.
    if (chunkIndex == 0) geometry_.releaseEmptyPages();

    // Batch all GPU uploads (VBs, IBs, textures) into a single command buffer
    // submission with one fence wait, instead of one per buffer/texture.
    vkCtx->beginUploadBatch();
    // Made with the tile's first chunks, before any of them is drawn.
    const std::shared_ptr<const TerrainTileMaps> maps = tileMapsFor(tileX, tileY, mesh);

    int uploaded = 0;
    while (chunkIndex < 256 && uploaded < maxChunksPerCall) {
        int cy = chunkIndex / 16;
        int cx = chunkIndex % 16;
        chunkIndex++;

        const auto& chunk = mesh.getChunk(cx, cy);
        if (!chunk.isValid()) continue;

        TerrainChunkGPU gpuChunk = uploadChunk(chunk);
        if (!gpuChunk.isValid()) continue;
        gpuChunk.maps = {maps, TerrainChunkSlot::at(cx, cy)};

        calculateBoundingSphere(gpuChunk, chunk);

        bindChunkTextures(gpuChunk, chunk, texturePaths, tileX, tileY, cx, cy);

        gpuChunk.tileX = tileX;
        gpuChunk.tileY = tileY;

        if (!writeChunkRow(gpuChunk)) {
            // The fallback textures are unsampleable, which does not pass, so
            // the chunk is dropped rather than asked for again.
            destroyChunkGPU(gpuChunk);
            continue;
        }

        registerRtChunk(gpuChunk, chunk);
        addChunk(std::move(gpuChunk));
        uploaded++;
    }

    vkCtx->endUploadBatch();

    return chunkIndex >= 256;
}

void TerrainRenderer::bindChunkTextures(TerrainChunkGPU& gpuChunk,
                                        const pipeline::ChunkMesh& chunk,
                                        const std::vector<std::string>& texturePaths,
                                        int tileX, int tileY, int chunkX, int chunkY) {
    if (chunk.layers.empty()) {
        gpuChunk.baseTexture = whiteTexture;
        return;
    }

    uint32_t baseTexId = chunk.layers[0].textureId;
    if (baseTexId < texturePaths.size()) {
        gpuChunk.baseTexture = loadTexture(texturePaths[baseTexId]);
        gpuChunk.baseNormalMap = loadNormalMap(texturePaths[baseTexId]);
    } else {
        LOG_WARNING("Terrain[", tileX, ",", tileY, "] chunk[", chunkX, ",", chunkY,
                    "] base textureId ", baseTexId, " >= texturePaths size ",
                    texturePaths.size(), " - white fallback");
        gpuChunk.baseTexture = whiteTexture;
    }

    // Layer 0 is the base, so the three blended layers are 1..3.
    for (size_t i = 1; i < chunk.layers.size() && i < 4; i++) {
        const auto& layer = chunk.layers[i];
        int li = static_cast<int>(i) - 1;

        TextureRef layerTex = whiteTexture;
        if (layer.textureId < texturePaths.size()) {
            layerTex = loadTexture(texturePaths[layer.textureId]);
            gpuChunk.layerNormalMaps[li] = loadNormalMap(texturePaths[layer.textureId]);
        } else {
            LOG_WARNING("Terrain[", tileX, ",", tileY, "] chunk[", chunkX, ",", chunkY,
                        "] layer[", i, "] textureId ", layer.textureId,
                        " >= texturePaths size ", texturePaths.size(),
                        " - white fallback");
        }
        gpuChunk.layerTextures[li] = std::move(layerTex);
        // What the layer covers is in the tile's maps (terrainTileMapPixels).
        gpuChunk.layerCount = static_cast<int>(i);
    }
}

TerrainChunkGPU TerrainRenderer::uploadChunk(const pipeline::ChunkMesh& chunk) {
    TerrainChunkGPU gpuChunk;

    gpuChunk.worldX = chunk.worldX;
    gpuChunk.worldY = chunk.worldY;
    gpuChunk.worldZ = chunk.worldZ;
    gpuChunk.indexCount = static_cast<uint32_t>(chunk.indices.size());

    // One index list for every level: the full mesh first, where the shadow
    // pass reads it, then the coarser levels and the skirts after it.
    std::vector<pipeline::TerrainIndex> indices;
    indices.reserve(chunk.indices.size() + chunk.lodIndices.size());
    indices.insert(indices.end(), chunk.indices.begin(), chunk.indices.end());
    indices.insert(indices.end(), chunk.lodIndices.begin(), chunk.lodIndices.end());
    gpuChunk.lods[0] = {0, gpuChunk.indexCount};
    for (size_t level = 1; level < gpuChunk.lods.size(); ++level) {
        const auto& range = chunk.lods[level];
        gpuChunk.lods[level] = {gpuChunk.indexCount + range.first, range.count};
    }
    for (size_t level = 0; level < gpuChunk.skirts.size(); ++level) {
        for (size_t edge = 0; edge < 4; ++edge) {
            const auto& range = chunk.skirts[level][edge];
            gpuChunk.skirts[level][edge] = {gpuChunk.indexCount + range.first, range.count};
        }
    }
    gpuChunk.chunkX = chunk.chunkX;
    gpuChunk.chunkY = chunk.chunkY;
    gpuChunk.lodErrors = chunk.lodError;
    gpuChunk.offeredLods = chunk.offeredLods();
    // Over the 145 grid vertices; the skirts after them hang straight down.
    for (size_t v = 0; v < std::min<size_t>(chunk.vertices.size(), 145); ++v) {
        gpuChunk.steepestUpZ = std::min(gpuChunk.steepestUpZ, std::abs(chunk.vertices[v].normal[2]));
    }

    // Not placed: the chunk is invalid and not drawn; the pool said why.
    gpuChunk.geometry = geometry_.place(chunk.vertices.data(), static_cast<uint32_t>(chunk.vertices.size()),
                                        indices.data(), static_cast<uint32_t>(indices.size()));
    if (gpuChunk.geometry.placed()) {
        const auto page = static_cast<size_t>(gpuChunk.geometry.page);
        if (pageChunks_.size() <= page) pageChunks_.resize(page + 1, 0);
        ++pageChunks_[page];
    }
    return gpuChunk;
}

TextureRef TerrainRenderer::loadTexture(const std::string& path) {
    TextureCache& cache = vkCtx->textures();
    const TextureKey key{normalizeTexturePath(path), TextureForm::Stored, 0};
    if (TextureRef cached = cache.find(key)) return cached;

    // Terrain tileset textures are sampled and never read back, so they go
    // up as blocks with the file's own mip levels.
    pipeline::BLPImage blp = assetManager->loadTexture(key.path, true);
    if (!blp.isValid()) {
        // White, and asked for again by the next tile that names it, in case
        // the asset becomes available.
        if (loggedTextureLoadFails_.insert(key.path).second) {
            LOG_WARNING("Failed to load texture: ", path, " - white ground where it is laid");
        }
        return whiteTexture;
    }
    // Refused or failed: the cache has said so.
    if (TextureRef uploaded = cacheStoredTexture(*vkCtx, key.path, blp)) return uploaded;
    return whiteTexture;
}

EncodedNormalMap TerrainRenderer::deriveNormalMap(const pipeline::BLPImage& blp, NormalMapFormat format) {
    if (!blp.isValid()) return {};
    const std::vector<uint8_t> decoded =
        blp.isBlockCompressed() ? pipeline::BLPLoader::decodeBaseLevel(blp) : blp.data;
    const auto width = static_cast<uint32_t>(blp.width);
    const auto height = static_cast<uint32_t>(blp.height);
    if (decoded.size() < static_cast<size_t>(width) * height * 4) return {};
    float variance = 0.0f;
    std::vector<uint8_t> rgba =
        generateNormalHeightMap(decoded.data(), width, height, kTerrainNormalMapStrength, variance);
    if (rgba.empty()) return {};
    return encodeNormalMap(std::move(rgba), width, height, format, variance);
}

TextureKey TerrainRenderer::normalMapKey(const std::string& path) {
    return {normalizeTexturePath(path), TextureForm::NormalHeight,
            static_cast<uint8_t>(kTerrainNormalMapStrength)};
}

NormalMapFormat TerrainRenderer::normalMapFormat() const {
    return vkCtx ? normalMapFormatFor(*vkCtx) : NormalMapFormat::Rgba8;
}

void TerrainRenderer::uploadPreloadedNormalMap(const std::string& path, const EncodedNormalMap& map) {
    // Held by nothing until a chunk binds it, as the textures above are.
    const TextureKey key = normalMapKey(path);
    if (!map.valid() || vkCtx->textures().find(key)) return;
    (void)cacheNormalMap(*vkCtx, key, map);
}

TextureRef TerrainRenderer::loadNormalMap(const std::string& path) {
    const TextureKey key = normalMapKey(path);
    if (TextureRef cached = vkCtx->textures().find(key)) return cached;
    if (loggedNormalMapFails_.count(key.path) != 0) return {};

    // The loading worker derives it ahead of the tile; it is missing here
    // only when the cache let it go before the chunk was bound.
    const EncodedNormalMap map =
        deriveNormalMap(assetManager->loadTexture(key.path, true), normalMapFormat());
    if (!map.valid()) {
        loggedNormalMapFails_.insert(key.path);
        LOG_WARNING("Terrain: no relief could be derived from '", path,
                    "' - the ground there is lit flat");
        return {};
    }
    // Refused or failed: the cache has said so, and the ground is lit flat.
    return cacheNormalMap(*vkCtx, key, map);
}

void TerrainRenderer::uploadPreloadedTextures(
    const std::unordered_map<std::string, pipeline::BLPImage>& textures) {
    // Into the shared cache ahead of the chunks that will name them, in one
    // submission. Held by nothing until those chunks bind them, so a full
    // cache may evict one again before then; loadTexture then reads it anew.
    TextureCache& cache = vkCtx->textures();
    vkCtx->beginUploadBatch();
    for (const auto& [path, blp] : textures) {
        if (!blp.isValid()) continue;
        if (cache.find({normalizeTexturePath(path), TextureForm::Stored, 0})) continue;
        (void)cacheStoredTexture(*vkCtx, path, blp);
    }
    vkCtx->endUploadBatch();
}

bool TerrainRenderer::writeChunkRow(TerrainChunkGPU& chunk) {
    // Valid, not merely non-null, and in the table. A texture whose upload or
    // view creation failed has a null view, and sampling that is undefined
    // behaviour that reaches an NVIDIA driver as a graphics engine exception
    // and a lost device rather than as anything this client can catch. See
    // #123. One the full table had no place for (it said so) has no handle.
    //
    // A chunk texture failing is ordinary - it is what the cache does under
    // memory pressure - and the 1x1 fallbacks are what that case is for.
    const auto sampleable = [](const TextureRef& t) {
        return t && t->isValid() && t.tableHandle() != kNoTextureHandle;
    };
    const auto pick = [&](const TextureRef& wanted, const TextureRef& fallback) {
        return sampleable(wanted) ? wanted.tableHandle() : fallback.tableHandle();
    };
    const TextureRef& white = whiteTexture;
    if (!sampleable(white)) {
        // The fallbacks themselves are gone, so there is nothing safe to write.
        // The caller drops the chunk: a hole in the ground costs a view of one
        // tile, and a null image view costs the device.
        static bool told = false;
        if (!told) {
            told = true;
            LOG_ERROR("TerrainRenderer: the 1x1 fallback textures are not "
                      "sampleable, so no chunk can be drawn");
        }
        return false;
    }

    // The flat normal is the relief's fallback, and a white one stands in
    // for it: lit flat but for a slight tilt, never unbound.
    const TextureRef& flat = sampleable(flatNormalTexture) ? flatNormalTexture : white;

    TerrainChunkRowGPU row;
    row.color[0] = pick(chunk.baseTexture, white);
    row.relief[0] = pick(chunk.baseNormalMap, flat);
    for (uint32_t i = 0; i < 3; i++) {
        row.color[1 + i] = pick(chunk.layerTextures[i], white);
        row.relief[1 + i] = pick(chunk.layerNormalMaps[i], flat);
    }
    // The tile's maps, or the fallback ones a tile whose maps failed was given.
    const TerrainTileMaps& maps = chunk.maps.tile ? *chunk.maps.tile : *fallbackTileMaps_;
    row.maps[0] = maps.coverHandle;
    row.maps[1] = maps.normalsHandle;
    // The tile's bake atlas, or the fallbacks where there is none; a chunk
    // with no atlas is never baked, and so never drawn from it.
    if (const TerrainBake::Atlas* atlas = bakeEnabled_ ? bake_.atlas(chunk.tileX, chunk.tileY) : nullptr) {
        row.maps[2] = atlas->colorHandle;
        row.maps[3] = atlas->reliefHandle;
    } else {
        row.maps[2] = white.tableHandle();
        row.maps[3] = flat.tableHandle();
    }
    row.layerCount = chunk.layerCount;
    row.mapLayer = static_cast<int32_t>(chunk.maps.slot.layer());

    if (chunk.row == TerrainChunkGPU::kNoRow) {
        if (freeRows_.empty()) {
            chunk.row = rowCount_++;
        } else {
            chunk.row = freeRows_.back();
            freeRows_.pop_back();
        }
    }
    // Queued with its row for the cull once it is among the chunks
    // (addChunk), where its neighbours are known.
    chunk.shading = row;
    return true;
}

namespace {


/// A tile's place on the map, as a key.
uint64_t chunkMapKey(int gx, int gy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(gx)) << 32) | static_cast<uint32_t>(gy);
}

}  // namespace

TerrainRenderer::GeometryDraw TerrainRenderer::geometryOf(const TerrainChunkGPU& chunk) const {
    GeometryDraw out;
    out.vertices = geometry_.vertices(chunk.geometry.page);
    out.indices = geometry_.indices(chunk.geometry.page);
    out.firstIndex = chunk.geometry.firstIndex;
    out.baseVertex = chunk.geometry.baseVertex;
    out.count = chunk.indexCount;
    return out;
}

void TerrainRenderer::bindDraw(VkCommandBuffer cmd, const GeometryDraw& draw) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &draw.vertices, &offset);
    vkCmdBindIndexBuffer(cmd, draw.indices, 0, VK_INDEX_TYPE_UINT32);
}

TerrainRenderer::DrawList TerrainRenderer::extract(const Camera& camera, const std::vector<LocalLight>& lights,
                                                   const WaterReflectionView* mirror) {
    // Its steps timed apart under the frame's own terrain stage: the walk
    // over every resident chunk against what each visible one costs.
    CpuStageLaps laps(vkCtx ? &vkCtx->gameCpuStages() : nullptr, "extract/terrain");
    DrawList list;
    list.wireframe = wireframe;
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());
    const glm::vec3 camPos = camera.getPosition();
    const uint32_t height = lodViewportHeight_ ? lodViewportHeight_ : vkCtx->getSwapchainExtent().height;
    const float focalPixels = std::abs(camera.getProjectionMatrix()[1][1]) * static_cast<float>(height) * 0.5f;
    const TerrainBakeBand band = terrainBakeBand(core::coords::TILE_SIZE / 16.0f, focalPixels);
    list.depth.reserve(shadowTiles_.size());
    for (auto& tile : shadowTiles_) {
        list.depth.push_back(tile.depth.extract(tile.chunks, [&](uint32_t index) {
            const auto& ch = chunks[index];
            return DepthDraw{geometryOf(ch), ch.boundingSphereCenter, ch.boundingSphereRadius};
        }));
    }
    laps.lap("depth lists");
    // Which chunks the view and the water's reflection see, at what level,
    // with which skirts, lights and surface, is the cull's on the GPU
    // (terrain_cull.comp.glsl), over every row; here only what it reads of
    // the frame. The rules are the ones the walk here used: Frustum and
    // WaterReflectionView, terrainLodFor, terrainSurfaceSourceFor,
    // pickLocalLights, terrainChunkShortOfDiscards.
    TerrainCullFrameGPU& cull = list.cull;
    const auto planeOf = [](const Plane& p) { return glm::vec4(p.normal, p.distance); };
    for (int i = 0; i < 6; ++i) cull.viewPlanes[i] = planeOf(frustum.getPlane(static_cast<Frustum::Side>(i)));
    if (mirror) {
        for (int i = 0; i < 6; ++i) cull.mirrorPlanes[i] = planeOf(mirror->seen.getPlane(static_cast<Frustum::Side>(i)));
    }
    cull.camera = glm::vec4(camPos, focalPixels);
    cull.limits = glm::vec4(maxViewDistance_, std::min(maxViewDistance_, kDistantTerrainStart), band.start, band.end);
    cull.mirror = glm::vec4(mirror ? 1.0f : 0.0f, mirror ? mirror->height : 0.0f,
                            normalMappingEnabled_ ? normalMapStrength_ : 0.0f, frustumCullingEnabled ? 1.0f : 0.0f);
    cull.counts = glm::uvec4(rowCount_, static_cast<uint32_t>(lights.size()), lodEnabled_ ? 1u : 0u,
                             bakeEnabled_ ? 1u : 0u);
    // Each page's lists have room for five commands a chunk it holds: its
    // level and up to four skirts.
    list.groups.resize(geometry_.pageCount());
    list.regions.resize(geometry_.pageCount() * 4);
    for (size_t g = 0; g < geometry_.pageCount(); ++g) {
        GroupDraw& group = list.groups[g];
        group.vertices = geometry_.vertices(static_cast<int32_t>(g));
        group.indices = geometry_.indices(static_cast<int32_t>(g));
        group.capacity = (g < pageChunks_.size() ? pageChunks_[g] : 0u) * 5u;
        for (size_t l = 0; l < 4; ++l) {
            list.regions[g * 4 + l] = list.commandCapacity;
            list.commandCapacity += group.capacity;
        }
    }
    cull.groups = glm::uvec4(static_cast<uint32_t>(list.groups.size()), 0u, 0u, 0u);
    laps.lap("cull frame");
    if (bakeEnabled_) {
        list.atlases = bake_.extractNewAtlases();
        // A skipped frame keeps its jobs pending. Recording acknowledges them
        // through a completion token, never by writing into the live chunk.
        // A chunk baked is read from its bake by the cull from now on.
        std::erase_if(bakeQueue_, [&](const std::pair<int, int>& place) {
            const uint32_t found = chunkIndex_.find(place.first, place.second);
            if (found == TerrainChunkIndex::kNone) return true;
            if (!chunks[found].bakeComplete->load()) return false;
            queueRows(found);
            return true;
        });
        const size_t take = std::min(bakeQueue_.size(), kBakeChunksPerFrame);
        for (size_t i = 0; i < take; ++i) {
            const auto& ch = chunks[chunkIndex_.find(bakeQueue_[i].first, bakeQueue_[i].second)];
            auto* atlas = bake_.atlas(ch.tileX, ch.tileY);
            if (!atlas) continue; // atlas() reports the allocation failure.
            list.bake.push_back({geometryOf(ch), ch.row, ch.tileX, ch.tileY,
                                 ch.chunkX, ch.chunkY, atlas, ch.bakeComplete});
        }
        std::sort(list.bake.begin(), list.bake.end(), [](const BakeDraw& a, const BakeDraw& b) {
            return std::pair(a.tileX, a.tileY) < std::pair(b.tileX, b.tileY);
        });
    }
    laps.lap("bake");

    // Every row not yet known to be on the GPU goes with every frame until
    // one that carried it is submitted; a frame dropped on the way loses none.
    const uint64_t submitted = rowsSubmitted_.load(std::memory_order_acquire);
    std::erase_if(pendingRows_, [submitted](const auto& entry) { return entry.second.extraction <= submitted; });
    ++extractions_;
    list.rowsExtraction = extractions_;
    list.rowCount = rowCount_;
    list.rowSlots.reserve(pendingRows_.size());
    list.rows.reserve(pendingRows_.size());
    list.cullRows.reserve(pendingRows_.size());
    for (const auto& [slot, pending] : pendingRows_) {
        list.rowSlots.push_back(slot);
        list.rows.push_back(pending.row);
        list.cullRows.push_back(pending.cull);
    }
    return list;
}

void TerrainRenderer::queueRows(uint32_t index) {
    const TerrainChunkGPU& chunk = chunks[index];
    if (chunk.row == TerrainChunkGPU::kNoRow || !chunk.isValid()) return;
    TerrainCullRowGPU cull;
    cull.sphere = glm::vec4(chunk.boundingSphereCenter, chunk.boundingSphereRadius);
    for (size_t level = 0; level < 4; ++level) cull.lodErrors[static_cast<int>(level)] = chunk.lodErrors[level];
    for (size_t edge = 0; edge < TerrainChunkIndex::kEdges; ++edge) {
        const uint32_t neighbour = chunkIndex_.neighbour(index, edge);
        cull.neighbours[edge] = neighbour == TerrainChunkIndex::kNone ? ~0u : chunks[neighbour].row;
    }
    cull.group = static_cast<uint32_t>(chunk.geometry.page);
    cull.firstIndex = chunk.geometry.firstIndex;
    cull.baseVertex = chunk.geometry.baseVertex;
    cull.offeredLods = chunk.offeredLods;
    cull.cell = static_cast<uint32_t>(chunk.chunkX + 16 * chunk.chunkY);
    cull.flags = TerrainCullRowGPU::kValid | (chunk.bakeComplete->load() ? TerrainCullRowGPU::kBaked : 0u);
    cull.steepestUpZ = chunk.steepestUpZ;
    for (size_t level = 0; level < 4; ++level) {
        cull.lods[level][0] = chunk.lods[level].first;
        cull.lods[level][1] = chunk.lods[level].count;
        for (size_t edge = 0; edge < 4; ++edge) {
            cull.skirts[level * 4 + edge][0] = chunk.skirts[level][edge].first;
            cull.skirts[level * 4 + edge][1] = chunk.skirts[level][edge].count;
        }
    }
    pendingRows_[chunk.row] = {chunk.shading, cull, extractions_ + 1};
}

void TerrainRenderer::uploadChunkRows(VkCommandBuffer cmd, uint32_t frameIndex, const DrawList& list) {
    cullFrames_[frameIndex].culled = false;
    if (list.rowCount == 0) return;
    if (!chunkRows_.reserve(cmd, list.rowCount) || !cullRows_.reserve(cmd, list.rowCount)) return;
    if (!chunkRows_.write(cmd, frameIndex, list.rowSlots, list.rows.data()) ||
        !cullRows_.write(cmd, frameIndex, list.rowSlots, list.cullRows.data())) {
        return;
    }
    rowsUploaded_ = list.rowsExtraction;
}

namespace {

// A buffer the cull writes or reads, at least `bytes` large; replaced (the
// old one released after the frames in flight) when it is not.
bool ensureCullBuffer(VkContext& ctx, AllocatedBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                      VmaMemoryUsage memory, const char* what) {
    if (buffer.buffer && buffer.info.size >= bytes) return true;
    DeferredResources retire(ctx);
    if (buffer.buffer) retire.buffer(buffer);
    buffer = createBuffer(ctx.getAllocator(), std::max<VkDeviceSize>(bytes, 256), usage, memory);
    if (buffer.buffer) return true;
    static core::LogBudget budget(4, "terrain cull buffers");
    if (budget.take())
        LOG_ERROR("Terrain cull: no memory for its ", what, " (", bytes, " bytes); the ground is not drawn");
    return false;
}

}  // namespace

bool TerrainRenderer::createCull() {
    VkDevice device = vkCtx->getDevice();
    countFromGpu_ = vkCtx->isDrawIndirectCountSupported();
    if (!countFromGpu_) {
        LOG_WARNING("Terrain: the device has no drawIndirectCount; each page's lists of ground draws are "
                    "drawn at their full room, five commands a chunk, the unused ones empty");
    }
    // terrain_cull.comp.glsl: the frame's view at 1, the rest storage.
    std::vector<VkDescriptorSetLayoutBinding> bindings(9);
    for (uint32_t b = 0; b < 9; ++b) {
        bindings[b] = {.binding = b,
                       .descriptorType = b == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       .descriptorCount = 1,
                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    cullSetLayout_ = createDescriptorSetLayout(device, bindings);
    const VkDescriptorPoolSize sizes[] = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 8 * MAX_FRAMES_IN_FLIGHT},
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    if (!cullSetLayout_ || vkCreateDescriptorPool(device, &poolInfo, nullptr, &cullSetPool_) != VK_SUCCESS) {
        LOG_ERROR("TerrainRenderer: could not make the cull's sets; the ground is not drawn");
        return false;
    }
    for (auto& frame : cullFrames_) {
        VkDescriptorSetAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = cullSetPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &cullSetLayout_;
        if (vkAllocateDescriptorSets(device, &allocInfo, &frame.set) != VK_SUCCESS) {
            LOG_ERROR("TerrainRenderer: could not allocate the cull's sets; the ground is not drawn");
            return false;
        }
    }
    const VkPushConstantRange pass{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(uint32_t)};
    cullPipelineLayout_ = createPipelineLayout(device, {cullSetLayout_}, {pass});
    VkShaderModule shader;
    if (!cullPipelineLayout_ || !shader.loadFromFile(device, "assets/shaders/terrain_cull.comp.spv")) {
        LOG_ERROR("TerrainRenderer: terrain_cull.comp.spv or its layout is missing; the ground is not drawn");
        return false;
    }
    VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    info.layout = cullPipelineLayout_;
    const VkResult made = vkCreateComputePipelines(device, vkCtx->getPipelineCache(), 1, &info, nullptr,
                                                   &cullPipeline_);
    shader.destroy();
    if (made != VK_SUCCESS) {
        LOG_ERROR("TerrainRenderer: the cull pipeline could not be made (VkResult ", static_cast<int>(made),
                  "); the ground is not drawn");
        return false;
    }
    return true;
}

void TerrainRenderer::destroyCull() {
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();
    for (auto& frame : cullFrames_) {
        for (AllocatedBuffer* buffer : {&frame.levels, &frame.records, &frame.commands, &frame.regions, &frame.counts,
                                        &frame.stats, &frame.view}) {
            if (buffer->buffer) destroyBuffer(allocator, *buffer);
        }
        frame = CullFrame{};
    }
    cullRows_.destroy();
    destroy(device, cullPipeline_);
    destroy(device, cullPipelineLayout_);
    destroy(device, cullSetPool_);
    destroy(device, cullSetLayout_);
}

void TerrainRenderer::dispatchCull(VkCommandBuffer cmd, uint32_t frameIndex, const DrawList& list, ::VkBuffer lights,
                                   uint32_t lightCount) {
    CullFrame& frame = cullFrames_[frameIndex];
    // What this slot's last cull counted: its frame has finished on the GPU.
    if (frame.counted && frame.stats.info.pMappedData) {
        TerrainCullStatsGPU counted;
        std::memcpy(&counted, frame.stats.info.pMappedData, sizeof(counted));
        renderedChunks = static_cast<int>(counted.drawn);
        culledChunks = static_cast<int>(counted.culled);
        bakedDrawnChunks_ = static_cast<int>(counted.baked);
        crossfadeDrawnChunks_ = static_cast<int>(counted.crossfade);
        std::memcpy(&furthestDrawnSq_, &counted.furthestSq, sizeof(float));
        renderedTriangles_ = counted.triangles;
        renderedDraws_ = counted.draws;
        frame.counted = false;
    }
    frame.culled = false;
    if (!cullPipeline_ || list.rowCount == 0 || !chunkRows_.buffer() || !cullRows_.buffer() ||
        rowsUploaded_ != list.rowsExtraction || !lights) {
        return;
    }
    const uint32_t groups = std::max<uint32_t>(1, static_cast<uint32_t>(list.groups.size()));
    const uint32_t commands = std::max<uint32_t>(1, list.commandCapacity);
    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bool replaced = !frame.setsCurrent;
    const auto ensure = [&](AllocatedBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                            VmaMemoryUsage memory, const char* what) {
        const ::VkBuffer before = buffer.buffer;
        if (!ensureCullBuffer(*vkCtx, buffer, bytes, usage, memory, what)) return false;
        replaced |= buffer.buffer != before;
        return true;
    };
    if (!ensure(frame.levels, VkDeviceSize(list.rowCount) * sizeof(int32_t), kStorage, VMA_MEMORY_USAGE_GPU_ONLY,
                "levels") ||
        !ensure(frame.records, VkDeviceSize(list.rowCount) * sizeof(TerrainDrawRecordGPU), kStorage,
                VMA_MEMORY_USAGE_GPU_ONLY, "draw records") ||
        !ensure(frame.commands, VkDeviceSize(commands) * sizeof(VkDrawIndexedIndirectCommand),
                kStorage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY, "commands") ||
        !ensure(frame.regions, VkDeviceSize(groups) * 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU, "list starts") ||
        !ensure(frame.counts, VkDeviceSize(groups) * 4 * sizeof(uint32_t),
                kStorage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY, "list counts") ||
        !ensure(frame.stats, sizeof(TerrainCullStatsGPU), kStorage, VMA_MEMORY_USAGE_GPU_TO_CPU, "statistics") ||
        !ensure(frame.view, sizeof(TerrainCullFrameGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU, "frame view")) {
        return;
    }
    // This slot's sets are rewritten after its fence, before anything binds
    // them, when a buffer or a table they name was replaced.
    if (replaced || frame.cullRowsVersion != cullRows_.version() || frame.chunkRowsVersion != chunkRows_.version()) {
        const VkDescriptorBufferInfo infos[] = {
            {cullRows_.buffer(), 0, cullRows_.bytes()},
            {frame.view.buffer, 0, sizeof(TerrainCullFrameGPU)},
            {frame.levels.buffer, 0, VK_WHOLE_SIZE},
            {frame.records.buffer, 0, VK_WHOLE_SIZE},
            {frame.commands.buffer, 0, VK_WHOLE_SIZE},
            {frame.regions.buffer, 0, VK_WHOLE_SIZE},
            {frame.counts.buffer, 0, VK_WHOLE_SIZE},
            {frame.stats.buffer, 0, VK_WHOLE_SIZE},
            {lights, 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet writes[11] = {};
        for (uint32_t b = 0; b < 9; ++b) {
            writes[b] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = frame.set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &infos[b];
        }
        const VkDescriptorBufferInfo chunkInfos[] = {{chunkRows_.buffer(), 0, chunkRows_.bytes()},
                                                     {frame.records.buffer, 0, VK_WHOLE_SIZE}};
        for (uint32_t b = 0; b < 2; ++b) {
            writes[9 + b] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[9 + b].dstSet = chunkSets_[frameIndex];
            writes[9 + b].dstBinding = b;
            writes[9 + b].descriptorCount = 1;
            writes[9 + b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[9 + b].pBufferInfo = &chunkInfos[b];
        }
        vkUpdateDescriptorSets(vkCtx->getDevice(), 11, writes, 0, nullptr);
        frame.cullRowsVersion = cullRows_.version();
        frame.chunkRowsVersion = chunkRows_.version();
        frame.setsCurrent = true;
    }
    // The lights bound are this frame's, so their count is too.
    TerrainCullFrameGPU view = list.cull;
    view.counts.y = lightCount;
    std::memcpy(frame.view.info.pMappedData, &view, sizeof(view));
    std::memcpy(frame.regions.info.pMappedData, list.regions.data(), list.regions.size() * sizeof(uint32_t));

    // After every earlier frame's draws from these buffers, before this one's.
    const auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst,
                             VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = src;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dst;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };
    constexpr VkPipelineStageFlags2 kDraws = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                             VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier(kDraws | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, frame.counts.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, frame.stats.buffer, 0, VK_WHOLE_SIZE, 0);
    // Without the count from the GPU every list is drawn at its full room:
    // what the cull leaves of it must be empty commands.
    if (!countFromGpu_) vkCmdFillBuffer(cmd, frame.commands.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipelineLayout_, 0, 1, &frame.set, 0, nullptr);
    const uint32_t workgroups = (list.rowCount + 63) / 64;
    for (uint32_t second = 0; second < 2; ++second) {
        vkCmdPushConstants(cmd, cullPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(second), &second);
        vkCmdDispatch(cmd, workgroups, 1, 1);
        // The second pass reads every chunk's level the first wrote.
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | kDraws,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    frame.culled = true;
    frame.counted = true;
}

void TerrainRenderer::frameSubmitted() {
    if (rowsUploaded_ == 0) return;
    rowsSubmitted_.store(rowsUploaded_, std::memory_order_release);
    rowsUploaded_ = 0;
}

void TerrainRenderer::recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list) {
    recordCulled(cmd, perFrameSet, list, false);
}

void TerrainRenderer::recordReflection(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list) {
    recordCulled(cmd, perFrameSet, list, true);
}

void TerrainRenderer::recordCulled(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list,
                                   bool reflection) {
    const uint32_t frameIndex = vkCtx->getCurrentFrame();
    const CullFrame& frame = cullFrames_[frameIndex];
    // Without this frame's cull the ground is not drawn; what kept it said why.
    if (!pipeline || !frame.culled) return;
    const VkDescriptorSet sets[] = {perFrameSet, vkCtx->textureTable().set(), chunkSets_[frameIndex]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 3, sets, 0, nullptr);
    const glm::mat4 model(1.0f);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(model), &model);
    // Per page, the chunks short of the distance discards through the
    // pipeline whose shader cannot discard, the rest through the one that
    // can. Terrain is opaque and depth-tested, so the order between chunks
    // does not change what is drawn.
    const uint32_t firstList = reflection ? 2u : 0u;
    for (size_t g = 0; g < list.groups.size(); ++g) {
        const GroupDraw& group = list.groups[g];
        if (group.capacity == 0 || !group.vertices) continue;
        bindDraw(cmd, GeometryDraw{.vertices = group.vertices, .indices = group.indices});
        for (uint32_t discards = 0; discards < 2; ++discards) {
            VkPipeline chosen = pipeline;
            if (list.wireframe && wireframePipeline) chosen = wireframePipeline;
            else if (discards == 0 && shortOfDiscardsPipeline) chosen = shortOfDiscardsPipeline;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, chosen);
            const uint32_t at = static_cast<uint32_t>(g) * 4u + firstList + discards;
            const VkDeviceSize offset = VkDeviceSize(list.regions[at]) * sizeof(VkDrawIndexedIndirectCommand);
            if (countFromGpu_) {
                vkCmdDrawIndexedIndirectCount(cmd, frame.commands.buffer, offset, frame.counts.buffer,
                                              VkDeviceSize(at) * sizeof(uint32_t), group.capacity,
                                              sizeof(VkDrawIndexedIndirectCommand));
            } else {
                vkCmdDrawIndexedIndirect(cmd, frame.commands.buffer, offset, group.capacity,
                                         sizeof(VkDrawIndexedIndirectCommand));
            }
        }
    }
}

void TerrainRenderer::addChunk(TerrainChunkGPU&& chunk) {
    const int gx = chunk.tileX * 16 + chunk.chunkX, gy = chunk.tileY * 16 + chunk.chunkY;
    if (chunkIndex_.find(gx, gy) != TerrainChunkIndex::kNone) {
        // Its tile was left with chunks in place: drawn twice, and the links
        // between chunks no longer to be trusted there.
        LOG_ERROR("TerrainRenderer: a second chunk at map chunk (", gx, ", ", gy, ") of tile [", chunk.tileX, ",",
                  chunk.tileY, "]; the tile's earlier chunks were never removed");
    }
    chunks.push_back(std::move(chunk));
    const TerrainChunkGPU& added = chunks.back();
    const uint32_t index = chunkIndex_.add(gx, gy);
    const auto [slot, newTile] = tileAt_.try_emplace(chunkMapKey(added.tileX, added.tileY),
                                                     static_cast<uint32_t>(shadowTiles_.size()));
    if (newTile) {
        shadowTiles_.emplace_back();
        shadowTiles_.back().tileX = added.tileX;
        shadowTiles_.back().tileY = added.tileY;
    }
    ShadowTile& tile = shadowTiles_[slot->second];
    tile.chunks.push_back(index);
    tile.depth.invalidate();
    queueBake(added);
    // Its row for the cull names its neighbours, and theirs now name it.
    queueRows(index);
    for (size_t edge = 0; edge < TerrainChunkIndex::kEdges; ++edge) {
        const uint32_t neighbour = chunkIndex_.neighbour(index, edge);
        if (neighbour != TerrainChunkIndex::kNone) queueRows(neighbour);
    }
}

void TerrainRenderer::queueBake(const TerrainChunkGPU& chunk) {
    if (!bakeEnabled_) return;
    bakeQueue_.emplace_back(chunk.tileX * 16 + chunk.chunkX, chunk.tileY * 16 + chunk.chunkY);
}

void TerrainRenderer::recordBake(VkCommandBuffer cmd, const DrawList& list) {
    if (!cmd) return;
    bake_.prepareNewAtlases(cmd, list.atlases);
    const uint32_t frameIndex = vkCtx->getCurrentFrame();
    // Without this frame's rows nothing is baked; the chunks wait.
    if (!cullFrames_[frameIndex].culled) return;
    const VkDescriptorSet sets[] = {vkCtx->textureTable().set(), chunkSets_[frameIndex]};
    for (size_t begin = 0; begin < list.bake.size();) {
        size_t end = begin + 1;
        while (end < list.bake.size() && list.bake[end].atlas == list.bake[begin].atlas) ++end;
        bool needed = false;
        for (size_t i = begin; i < end; ++i) needed |= !list.bake[i].complete->load();
        if (needed) {
            bake_.beginTile(cmd, *list.bake[begin].atlas);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bake_.pipelineLayout(), 0, 2, sets, 0,
                                    nullptr);
            for (size_t i = begin; i < end; ++i) {
                const auto& draw = list.bake[i];
                if (draw.complete->load()) continue;
                bake_.cell(cmd, draw.chunkX, draw.chunkY);
                vkCmdPushConstants(cmd, bake_.pipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draw.row),
                                   &draw.row);
                bindDraw(cmd, draw.geometry);
                vkCmdDrawIndexed(cmd, draw.geometry.count, 1, draw.geometry.firstIndex,
                                  draw.geometry.baseVertex, 0);
                draw.complete->store(true);
            }
            bake_.endTile(cmd, *list.bake[begin].atlas);
        }
        begin = end;
    }
}

bool TerrainRenderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx || shadowRenderPass == VK_NULL_HANDLE) return false;
    if (shadowPipeline_ != VK_NULL_HANDLE) return true;  // already initialised
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    if (!createShadowParamsSet(device, allocator, sizeof(ShadowParamsUBO),
                               whiteTexture->getImageView(),
                               whiteTexture->getSampler(), "TerrainRenderer",
                               shadowParams_)) {
        return false;
    }

    // Pipeline layout: set 0 = shadowParams_.layout, push 128 bytes (lightSpaceMatrix + model)
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix and the flags
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParams_.layout}, {pc});
    if (!shadowPipelineLayout_) {
        LOG_ERROR("TerrainRenderer: failed to create shadow pipeline layout");
        return false;
    }

    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/shadow.vert.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load shadow fragment shader");
        vertShader.destroy();
        return false;
    }

    const VkVertexInputBindingDescription vertBind =
        perVertexBinding(sizeof(pipeline::TerrainVertex));
    const std::vector<VkVertexInputAttributeDescription> vertAttrs =
        toVkAttributes(kTerrainShadowVertexAttributes);

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("TerrainRenderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("TerrainRenderer shadow pipeline initialized");
    return true;
}

void TerrainRenderer::recordDepth(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                                  const glm::vec3& shadowCenter, float shadowRadius, const DrawList& list) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
                            0, 1, &shadowParams_.set, 0, nullptr);
    ShadowPush push{.lightSpaceModel = lightSpaceMatrix};
    vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(push), &push);
    ::VkBuffer boundVertices = VK_NULL_HANDLE, boundIndices = VK_NULL_HANDLE;
    visitTerrainDepth(list.depth, lightSpaceMatrix, shadowCenter, shadowRadius, [&](const GeometryDraw& geometry) {
        if (boundVertices != geometry.vertices || boundIndices != geometry.indices) {
            bindDraw(cmd, geometry);
            boundVertices = geometry.vertices;
            boundIndices = geometry.indices;
        }
        vkCmdDrawIndexed(cmd, geometry.count, 1, geometry.firstIndex, geometry.baseVertex, 0);
    });
}

void TerrainRenderer::removeTile(int tileX, int tileY) {
    // Only the tile's own chunks are touched: each is filled by the last chunk
    // of the list, and that one's place and its tile's entry are moved with
    // it. Erasing each of the 256 where it was found moved every chunk after
    // it, 256 times over a list of 30,000-40,000 - 200-250 ms on the main
    // thread for each tile left behind in flight.
    const auto tileIt = tileAt_.find(chunkMapKey(tileX, tileY));
    if (tileIt != tileAt_.end()) {
        const uint32_t tileSlot = tileIt->second;
        std::vector<uint32_t> leaving = std::move(shadowTiles_[tileSlot].chunks);
        // Highest first: everything past the place being filled has gone or
        // stays, so the chunk moved into it is never one still to leave.
        std::sort(leaving.begin(), leaving.end(), std::greater<>());
        // The chunks beside the tile lose a neighbour: their rows for the
        // cull are written again once the tile's chunks are gone.
        std::vector<std::pair<int, int>> beside;
        for (const uint32_t index : leaving) {
            const TerrainChunkGPU& chunk = chunks[index];
            const int gx = chunk.tileX * 16 + chunk.chunkX, gy = chunk.tileY * 16 + chunk.chunkY;
            beside.insert(beside.end(), {{gx, gy - 1}, {gx, gy + 1}, {gx - 1, gy}, {gx + 1, gy}});
        }
        for (const uint32_t index : leaving) {
            TerrainChunkGPU& chunk = chunks[index];
            chunkIndex_.removeSwapLast(index);
            destroyChunkGPU(chunk);
            const auto last = static_cast<uint32_t>(chunks.size() - 1);
            if (index != last) {
                chunk = std::move(chunks[last]);
                // The moved chunk keeps its geometry; the other tile's immutable
                // depth snapshot contains values, not these live array indices.
                auto& moversTile = shadowTiles_[tileAt_.at(chunkMapKey(chunk.tileX, chunk.tileY))].chunks;
                *std::find(moversTile.begin(), moversTile.end(), last) = index;
            }
            chunks.pop_back();
        }
        for (const auto& [gx, gy] : beside) {
            const uint32_t found = chunkIndex_.find(gx, gy);
            if (found != TerrainChunkIndex::kNone) queueRows(found);
        }
        // The tile's entry goes the same way.
        const auto lastSlot = static_cast<uint32_t>(shadowTiles_.size() - 1);
        if (tileSlot != lastSlot) {
            shadowTiles_[tileSlot] = std::move(shadowTiles_[lastSlot]);
            tileAt_[chunkMapKey(shadowTiles_[tileSlot].tileX, shadowTiles_[tileSlot].tileY)] = tileSlot;
        }
        shadowTiles_.pop_back();
        tileAt_.erase(chunkMapKey(tileX, tileY));
        LOG_DEBUG("Removed ", leaving.size(), " terrain chunks for tile [", tileX, ",", tileY, "]");
    }
    // Its chunks still queued find nothing at their place and are dropped.
    bake_.releaseTile(tileX, tileY);
    releaseTileMaps(tileX, tileY);
}

std::shared_ptr<const TerrainTileMaps> TerrainRenderer::tileMapsFor(int tileX, int tileY,
                                                                    const pipeline::TerrainMesh& mesh) {
    const uint64_t key = chunkMapKey(tileX, tileY);
    if (auto it = tileMaps_.find(key); it != tileMaps_.end()) return it->second;

    const TerrainTileMapPixels pixels = terrainTileMapPixels(mesh);
    if (pixels.chunksLitFlat > 0) {
        LOG_WARNING("Terrain[", tileX, ",", tileY, "]: ", pixels.chunksLitFlat,
                    " chunk(s) without a normal image - lit as flat ground");
    }

    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    auto made = std::make_shared<TerrainTileMaps>();
    made->cover = createArrayImage(device, allocator, TerrainTileMapPixels::kCoverSize,
                                   TerrainTileMapPixels::kCoverSize, TerrainChunkSlot::kCount,
                                   VK_FORMAT_R8G8B8A8_UNORM, usage);
    made->normals = createArrayImage(device, allocator, TerrainTileMapPixels::kNormalSize,
                                     TerrainTileMapPixels::kNormalSize, TerrainChunkSlot::kCount,
                                     VK_FORMAT_R8G8B8A8_UNORM, usage);
    if (!made->cover.imageView || !made->normals.imageView ||
        !uploadTileMapLayers(*vkCtx, *made, pixels.cover.data(), pixels.normals.data(),
                             TerrainTileMapPixels::kCoverSize, TerrainTileMapPixels::kNormalSize,
                             TerrainChunkSlot::kCount) ||
        !placeTileMaps(vkCtx->textureTable(), tileMapSampler_, *made)) {
        destroyTileMaps(device, allocator, vkCtx->textureTable(), *made);
        // Kept for the tile, so its later chunks take the same maps rather
        // than trying again; releaseTileMaps leaves the fallback alone.
        tileMaps_.emplace(key, fallbackTileMaps_);
        if (!tileMapsFailureSaid_) {
            tileMapsFailureSaid_ = true;
            LOG_WARNING("Terrain[", tileX, ",", tileY, "]: its chunk maps could not be made "
                        "(createImage said why); its layers are drawn over the whole of each chunk and "
                        "it is lit flat, as is any other tile that fails the same way");
        }
        return fallbackTileMaps_;
    }
    tileMaps_.emplace(key, made);
    return made;
}

void TerrainRenderer::releaseTileMaps(int tileX, int tileY) {
    const auto it = tileMaps_.find(chunkMapKey(tileX, tileY));
    if (it == tileMaps_.end()) return;
    std::shared_ptr<TerrainTileMaps> retired = std::move(it->second);
    tileMaps_.erase(it);
    if (retired == fallbackTileMaps_) return;
    // The chunks whose rows name it may still be drawn by the frames in
    // flight.
    vkCtx->deferAfterAllFrameFences([device = vkCtx->getDevice(), allocator = vkCtx->getAllocator(),
                                     table = &vkCtx->textureTable(), retired] {
        destroyTileMaps(device, allocator, *table, *retired);
    });
}

void TerrainRenderer::clear() {
    if (!vkCtx) return;

    for (auto& chunk : chunks) {
        destroyChunkGPU(chunk);
    }
    chunks.clear();
    chunkIndex_.clear();
    shadowTiles_.clear();
    tileAt_.clear();
    bakeQueue_.clear();
    bake_.releaseAll();
    while (!tileMaps_.empty()) {
        const uint64_t key = tileMaps_.begin()->first;
        releaseTileMaps(static_cast<int>(static_cast<int32_t>(key >> 32)),
                        static_cast<int>(static_cast<int32_t>(key & 0xffffffffu)));
    }
    renderedChunks = 0;
    renderedDraws_ = 0;
    // Every chunk gave its row back above; the table's rows are written again
    // as the next chunks arrive.
    freeRows_.clear();
    rowCount_ = 0;
    pendingRows_.clear();
}

void TerrainRenderer::registerRtChunk(TerrainChunkGPU& gpuChunk, const pipeline::ChunkMesh& chunk) {
    if (!rtScene_) return;
    RtScene::MeshSource src;
    src.positions.reserve(chunk.vertices.size());
    for (const auto& v : chunk.vertices) {
        src.positions.emplace_back(v.position[0], v.position[1], v.position[2]);
    }
    src.indices.assign(chunk.indices.begin(), chunk.indices.end());

    // Estimate RT albedo with the same absolute coverage as the main pass.
    const glm::vec3 base = gpuChunk.baseTexture ? gpuChunk.baseTexture->averageColor() : glm::vec3(0.5f);
    glm::vec3 overlays(0.0f);
    float coverage = 0.0f;
    for (int i = 0; i < 3 && i + 1 < static_cast<int>(chunk.layers.size()); ++i) {
        const VkTexture* tex = gpuChunk.layerTextures[i].get();
        const auto& alpha = chunk.layers[i + 1].alphaData;
        if (!tex) continue;
        float cover = 1.0f; // The renderer binds opaque alpha for an absent map.
        if (!alpha.empty()) {
            uint64_t sum = 0;
            for (uint8_t a : alpha) sum += a;
            cover = static_cast<float>(sum) / (255.0f * static_cast<float>(alpha.size()));
        }
        overlays += tex->averageColor() * cover;
        coverage += cover;
    }
    const glm::vec3 albedo = base * std::max(1.0f - coverage, 0.0f) +
                             overlays / std::max(coverage, 1.0f);
    src.surfaces.push_back(packRtSurface(albedo, 1.0f));

    gpuChunk.rtMesh = rtScene_->addMesh(std::move(src));
    // Terrain vertices are already in world space.
    gpuChunk.rtInstance = rtScene_->addInstance(gpuChunk.rtMesh, glm::mat4(1.0f));
}

void TerrainRenderer::destroyChunkGPU(TerrainChunkGPU& chunk) {
    if (!vkCtx) return;

    if (rtScene_ && chunk.rtMesh != RtScene::kInvalid) {
        rtScene_->removeInstance(chunk.rtInstance);
        rtScene_->removeMesh(chunk.rtMesh);
        chunk.rtInstance = chunk.rtMesh = RtScene::kInvalid;
    }

    // Its row may go to the next chunk at once: a frame in flight draws from
    // what the table holds now, and the next chunk's row is copied in by a
    // later frame, after that one's draws in the queue.
    // The cull must stop drawing it, from that frame on: its row goes as one
    // nothing draws, unless the next chunk given it writes it first.
    if (chunk.row != TerrainChunkGPU::kNoRow) {
        pendingRows_[chunk.row] = {TerrainChunkRowGPU{}, TerrainCullRowGPU{}, extractions_ + 1};
        freeRows_.push_back(chunk.row);
        chunk.row = TerrainChunkGPU::kNoRow;
    }
    if (chunk.geometry.placed() && static_cast<size_t>(chunk.geometry.page) < pageChunks_.size()) {
        --pageChunks_[static_cast<size_t>(chunk.geometry.page)];
    }
    // The chunk's ranges in its geometry page, given back once no frame in
    // flight draws them; until then nothing else is copied over them.
    geometry_.release(chunk.geometry);

    // The textures are let go below: the shared cache keeps a tileset texture
    // for the next tile that wants it, and retires it only after the frames
    // that may still sample it. The tile's maps go with the tile
    // (releaseTileMaps).

    chunk.baseTexture.reset();
    for (TextureRef& t : chunk.layerTextures) t.reset();
    chunk.baseNormalMap.reset();
    for (TextureRef& t : chunk.layerNormalMaps) t.reset();
    chunk.maps = {};
}

int TerrainRenderer::getTriangleCount() const {
    int total = 0;
    for (const auto& chunk : chunks) {
        total += chunk.indexCount / 3;
    }
    return total;
}

void TerrainRenderer::calculateBoundingSphere(TerrainChunkGPU& gpuChunk,
                                                const pipeline::ChunkMesh& meshChunk) {
    if (meshChunk.vertices.empty()) {
        gpuChunk.boundingSphereRadius = 0.0f;
        gpuChunk.boundingSphereCenter = glm::vec3(0.0f);
        return;
    }

    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());

    for (const auto& vertex : meshChunk.vertices) {
        glm::vec3 pos(vertex.position[0], vertex.position[1], vertex.position[2]);
        min = glm::min(min, pos);
        max = glm::max(max, pos);
    }

    gpuChunk.boundingSphereCenter = (min + max) * 0.5f;

    float maxDistSq = 0.0f;
    for (const auto& vertex : meshChunk.vertices) {
        glm::vec3 pos(vertex.position[0], vertex.position[1], vertex.position[2]);
        glm::vec3 diff = pos - gpuChunk.boundingSphereCenter;
        float distSq = glm::dot(diff, diff);
        maxDistSq = std::max(maxDistSq, distSq);
    }

    gpuChunk.boundingSphereRadius = std::sqrt(maxDistSq);
}

} // namespace rendering
} // namespace wowee
