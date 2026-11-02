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

// The fragment stage's push constants, after the vertex stage's
// GPUPushConstants: the surface, then the chunk's row. Matches `Surface` in
// terrain.frag.glsl.
constexpr uint32_t kSurfacePushOffset = sizeof(GPUPushConstants);
constexpr uint32_t kRowPushOffset = kSurfacePushOffset + sizeof(TerrainSurfacePush);
static_assert(kRowPushOffset + sizeof(uint32_t) <= 128,
              "the terrain's push constants fit the space Vulkan guarantees");

// The pipelines' sets: the frame's block, the texture table, the chunk rows.
constexpr uint32_t kTableSet = 1;
constexpr uint32_t kChunkSet = 2;

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

    // --- The chunk table's set, one per frame slot ---
    const VkDescriptorSetLayoutBinding chunkBinding{.binding = 0,
                                                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    .descriptorCount = 1,
                                                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    chunkSetLayout_ = createDescriptorSetLayout(device, {chunkBinding});
    const VkDescriptorPoolSize chunkPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .descriptorCount = MAX_FRAMES_IN_FLIGHT};
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
    const VkDescriptorSetLayout tableLayout = vkCtx->textureTable().layout();

    // The far ground's bake, which says itself why when it cannot be had.
    if (!core::envFlagEnabled("WOWEE_TERRAIN_BAKE", true)) {
        LOG_WARNING("WOWEE_TERRAIN_BAKE=0: every terrain chunk is drawn from its layers, however far");
    } else {
        bakeEnabled_ = bake_.initialize(*vkCtx, tableLayout, chunkSetLayout_);
    }

    // --- Create pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(GPUPushConstants);
    VkPushConstantRange surfaceRange{};
    surfaceRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    surfaceRange.offset = kSurfacePushOffset;
    surfaceRange.size = sizeof(TerrainSurfacePush) + sizeof(uint32_t);

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, tableLayout, chunkSetLayout_ };
    pipelineLayout = createPipelineLayout(device, setLayouts, { pushRange, surfaceRange });
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

    // The geometry pages are made as tiles arrive (placeInGeometryPage).

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
    chunkRows_.destroy();
    destroy(device, chunkSetPool_);
    destroy(device, chunkSetLayout_);
    for (auto& set : chunkSets_) set = VK_NULL_HANDLE;
    for (auto& version : chunkSetVersion_) version = 0;

    // Shadow pipeline cleanup
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroyShadowParamsSet(device, allocator, shadowParams_);

    // Every chunk's ranges came back in the flush above; the device is idle.
    destroyGeometryPages();

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

    releaseEmptyGeometryPages();
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
    if (chunkIndex == 0) releaseEmptyGeometryPages();

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
    gpuChunk.vertexCount = static_cast<uint32_t>(chunk.vertices.size());
    gpuChunk.allIndexCount = static_cast<uint32_t>(chunk.indices.size() + chunk.lodIndices.size());

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

    // Not placed: the chunk is invalid and not drawn; placeInGeometryPage
    // said why.
    placeInGeometryPage(gpuChunk, chunk.vertices, indices);
    return gpuChunk;
}

bool TerrainRenderer::placeInGeometryPage(TerrainChunkGPU& gpuChunk,
                                          const std::vector<pipeline::TerrainVertex>& vertices,
                                          const std::vector<pipeline::TerrainIndex>& indices) {
    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    const auto indexCount = static_cast<uint32_t>(indices.size());
    if (vertexCount == 0 || indexCount == 0) return false;
    const bool shareable = vertexCount <= kPageVertices && indexCount <= kPageIndices;

    // A shared page with room, else a new one in the first empty slot, else
    // a page of the chunk's own size.
    int32_t slot = -1;
    std::optional<uint32_t> firstVertex;
    std::optional<uint32_t> firstIndex;
    for (size_t i = 0; shareable && i < geometryPages_.size() && slot < 0; ++i) {
        TerrainGeometryPage* page = geometryPages_[i].get();
        if (!page || page->fitted || page->vertexRanges.largestFree() < vertexCount ||
            page->indexRanges.largestFree() < indexCount) {
            continue;
        }
        firstVertex = page->vertexRanges.allocate(vertexCount);
        firstIndex = page->indexRanges.allocate(indexCount);
        slot = static_cast<int32_t>(i);
    }
    const auto makePage = [&](uint32_t vertexRoom, uint32_t indexRoom, bool fitted) -> bool {
        auto page = std::make_shared<TerrainGeometryPage>();
        VmaAllocator allocator = vkCtx->getAllocator();
        page->vertices = createBuffer(allocator,
            static_cast<VkDeviceSize>(vertexRoom) * sizeof(pipeline::TerrainVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);
        page->indices = createBuffer(allocator,
            static_cast<VkDeviceSize>(indexRoom) * sizeof(pipeline::TerrainIndex),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);
        if (!page->vertices.buffer || !page->indices.buffer) {
            if (page->vertices.buffer) destroyBuffer(allocator, page->vertices);
            if (page->indices.buffer) destroyBuffer(allocator, page->indices);
            return false;
        }
        page->vertexRanges = RangeAllocator(vertexRoom);
        page->indexRanges = RangeAllocator(indexRoom);
        page->fitted = fitted;
        firstVertex = page->vertexRanges.allocate(vertexCount);
        firstIndex = page->indexRanges.allocate(indexCount);
        auto empty = std::find(geometryPages_.begin(), geometryPages_.end(), nullptr);
        if (empty == geometryPages_.end()) {
            geometryPages_.push_back(std::move(page));
            slot = static_cast<int32_t>(geometryPages_.size() - 1);
        } else {
            *empty = std::move(page);
            slot = static_cast<int32_t>(empty - geometryPages_.begin());
        }
        return true;
    };
    if (slot < 0 && shareable && !geometryPagesExhausted_) {
        if (makePage(kPageVertices, kPageIndices, false)) {
            LOG_INFO("Terrain: geometry page ", slot, " made (", geometryStats().pages,
                     " resident, ", kTilesPerGeometryPage, " tiles each)");
        } else {
            geometryPagesExhausted_ = true;
            LOG_WARNING("Terrain: no geometry page could be allocated (",
                        geometryStats().pages, " pages resident) - chunks get pages of their own"
                        " size until a page is let go, a draw group each");
        }
    }
    if (slot < 0 && !makePage(vertexCount, indexCount, true)) {
        static core::LogBudget budget(4, "terrain chunk geometry");
        if (budget.take())
            LOG_WARNING("Terrain: no memory for a chunk's own geometry page (", vertexCount, " vertices, ",
                        indexCount, " indices) - the chunk is not drawn");
        return false;
    }

    TerrainGeometryPage& page = *geometryPages_[slot];
    const bool copied =
        uploadIntoBuffer(*vkCtx, vertices.data(),
                         static_cast<VkDeviceSize>(vertexCount) * sizeof(pipeline::TerrainVertex),
                         page.vertices.buffer,
                         static_cast<VkDeviceSize>(*firstVertex) * sizeof(pipeline::TerrainVertex)) &&
        uploadIntoBuffer(*vkCtx, indices.data(),
                         static_cast<VkDeviceSize>(indexCount) * sizeof(pipeline::TerrainIndex),
                         page.indices.buffer,
                         static_cast<VkDeviceSize>(*firstIndex) * sizeof(pipeline::TerrainIndex));
    if (!copied) {
        // Nothing has drawn these ranges, so they come back at once.
        page.vertexRanges.free(*firstVertex, vertexCount);
        page.indexRanges.free(*firstIndex, indexCount);
        LOG_WARNING("Terrain: a chunk's geometry could not be staged into page ", slot,
                    " - the chunk is not drawn");
        return false;
    }
    gpuChunk.geometryPage = slot;
    gpuChunk.baseVertex = static_cast<int32_t>(*firstVertex);
    gpuChunk.firstIndex = *firstIndex;
    return true;
}

void TerrainRenderer::releaseEmptyGeometryPages() {
    // A page is empty only once every chunk in it has been retired behind the
    // frame fences, so nothing in flight reads it and it can go now.
    bool spareKept = false;
    VmaAllocator allocator = vkCtx->getAllocator();
    for (auto& page : geometryPages_) {
        if (!page || page->vertexRanges.used() != 0 || page->indexRanges.used() != 0) continue;
        if (!spareKept && !page->fitted) {
            spareKept = true;
            continue;
        }
        destroyBuffer(allocator, page->vertices);
        destroyBuffer(allocator, page->indices);
        page.reset();
        geometryPagesExhausted_ = false;
    }
    while (!geometryPages_.empty() && !geometryPages_.back()) geometryPages_.pop_back();
}

void TerrainRenderer::destroyGeometryPages() {
    VmaAllocator allocator = vkCtx->getAllocator();
    for (auto& page : geometryPages_) {
        if (!page) continue;
        destroyBuffer(allocator, page->vertices);
        destroyBuffer(allocator, page->indices);
    }
    geometryPages_.clear();
    geometryPagesExhausted_ = false;
}

TerrainGeometryStats TerrainRenderer::geometryStats() const {
    TerrainGeometryStats stats;
    for (const auto& page : geometryPages_) {
        if (!page) continue;
        ++stats.pages;
        stats.vertexCapacity += page->vertexRanges.capacity();
        stats.verticesUsed += page->vertexRanges.used();
        stats.indexCapacity += page->indexRanges.capacity();
        stats.indicesUsed += page->indexRanges.used();
        if (page->fitted) ++stats.fittedPages;
    }
    return stats;
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
    pendingRows_[chunk.row] = {row, extractions_ + 1};
    return true;
}

namespace {

/// The most a coarser level may move the ground on screen, in pixels: how
/// far its surface strays from the full mesh (ChunkMesh::lodError), seen
/// from the chunk's nearest point. Under a pixel the shape cannot be told
/// from the full mesh's; the light is the same at every level, from the
/// chunk's normal image.
constexpr float kLodMaxErrorPixels = 1.0f;

/// A tile's place on the map, as a key.
uint64_t chunkMapKey(int gx, int gy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(gx)) << 32) | static_cast<uint32_t>(gy);
}

}  // namespace

TerrainRenderer::GeometryDraw TerrainRenderer::geometryOf(const TerrainChunkGPU& chunk) const {
    GeometryDraw out;
    const auto& page = *geometryPages_[chunk.geometryPage];
    out.vertices = page.vertices.buffer;
    out.indices = page.indices.buffer;
    out.firstIndex = chunk.firstIndex;
    out.baseVertex = chunk.baseVertex;
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
    std::vector<int8_t> levels(chunks.size(), -1);
    std::vector<uint32_t> visible, mirrored;
    list.depth.reserve(shadowTiles_.size());
    for (auto& tile : shadowTiles_) {
        list.depth.push_back(tile.depth.extract(tile.chunks, [&](uint32_t index) {
            const auto& ch = chunks[index];
            return DepthDraw{geometryOf(ch), ch.boundingSphereCenter, ch.boundingSphereRadius};
        }));
    }
    laps.lap("depth lists");
    // Tiles first, by the sphere round all of a tile's chunks the depth list
    // just made: a chunk inside a tile that is out of reach, of view and of
    // the mirror is out of each of them too. The walk went over every chunk
    // resident - 34,500 in Dalaran - for the 5,400 it drew. The chunks of the
    // tiles kept are tested in their list order, as before, so the draws
    // come out the same.
    // Whether a sphere is in reach and in view, and whether the water's
    // reflection can show it - the far side of a hill over a lake, which the
    // camera may not see itself. One test for a tile's sphere and a chunk's:
    // a sphere inside another passes no test the other fails.
    struct Seen { float distanceSq; bool view, mirror; };
    const auto seen = [&](const glm::vec3& centre, float radius) {
        const glm::vec2 step = glm::vec2(centre - camPos);
        const float distanceSq = glm::dot(step, step);
        const float limit = maxViewDistance_ + radius;
        const bool inReach = distanceSq <= limit * limit;
        return Seen{distanceSq, inReach && (!frustumCullingEnabled || frustum.intersectsSphere(centre, radius)),
                    inReach && mirror && mirror->canSee(centre, radius)};
    };
    std::vector<uint64_t> toTest((chunks.size() + 63) / 64, 0);
    for (size_t t = 0; t < shadowTiles_.size(); ++t) {
        const Seen tile = seen(list.depth[t]->centre, list.depth[t]->radius);
        if (!tile.view && !tile.mirror) {
            // Every chunk resident is valid and has its material.
            list.culled += static_cast<int>(shadowTiles_[t].chunks.size());
            continue;
        }
        for (const uint32_t index : shadowTiles_[t].chunks) toTest[index / 64] |= uint64_t{1} << (index % 64);
    }
    std::vector<uint32_t> candidates;
    for (size_t word = 0; word < toTest.size(); ++word) {
        for (uint64_t bits = toTest[word]; bits != 0; bits &= bits - 1)
            candidates.push_back(static_cast<uint32_t>(word * 64 + static_cast<size_t>(std::countr_zero(bits))));
    }
    for (const uint32_t i : candidates) {
        const auto& ch = chunks[i];
        if (!ch.isValid()) continue;
        if (ch.row == TerrainChunkGPU::kNoRow) continue;
        const Seen chunk = seen(ch.boundingSphereCenter, ch.boundingSphereRadius);
        if (!chunk.view) ++list.culled;
        if (!chunk.view && !chunk.mirror) continue;
        const float nearest = glm::length(ch.boundingSphereCenter - camPos) - ch.boundingSphereRadius;
        levels[i] = static_cast<int8_t>(lodEnabled_ ? pipeline::terrainLodFor(ch.lodErrors, ch.offeredLods,
                                                     nearest, focalPixels, kLodMaxErrorPixels) : 0);
        if (chunk.view) {
            visible.push_back(i);
            list.furthestSq = std::max(list.furthestSq, chunk.distanceSq);
        }
        if (chunk.mirror) mirrored.push_back(i);
    }
    laps.lap("walk");
    // A chunk's draw, and where its surface is read from.
    const auto drawOf = [&](uint32_t i) -> std::pair<ChunkDraw, TerrainSurfaceSource> {
        const auto& ch = chunks[i];
        const size_t level = static_cast<size_t>(levels[i]);
        ChunkDraw draw;
        draw.row = ch.row;
        draw.geometry = geometryOf(ch);
        draw.ranges[0] = ch.lods[level];
        // A skirt where the neighbour across an edge is missing or drawn at
        // another level.
        for (size_t edge = 0; edge < TerrainChunkIndex::kEdges; ++edge) {
            const uint32_t neighbour = chunkIndex_.neighbour(i, edge);
            if (neighbour == TerrainChunkIndex::kNone || levels[neighbour] != levels[i])
                draw.ranges[edge + 1] = ch.skirts[level][edge];
        }
        const float distance = glm::length(ch.boundingSphereCenter - camPos);
        const auto source = bakeEnabled_ && ch.bakeComplete->load()
            ? terrainSurfaceSourceFor(band, distance - ch.boundingSphereRadius,
                                      distance + ch.boundingSphereRadius, ch.steepestUpZ)
            : TerrainSurfaceSource::Layers;
        draw.surface.baked = glm::vec4(static_cast<float>(source), band.start, band.end,
                                       source == TerrainSurfaceSource::Layers ? 0.0f : static_cast<float>(ch.chunkX + 16 * ch.chunkY));
        draw.surface.relief.x = normalMappingEnabled_ ? normalMapStrength_ : 0.0f;
        draw.lights = lights.empty() ? kNoLocalLights : pickLocalLights(lights, ch.boundingSphereCenter,
                                                                       ch.boundingSphereRadius);
        draw.reachesDiscards = !terrainChunkShortOfDiscards(
            glm::length(glm::vec2(ch.boundingSphereCenter - camPos)), ch.boundingSphereRadius,
            maxViewDistance_, kDistantTerrainStart);
        return {draw, source};
    };
    list.view.reserve(visible.size());
    for (const uint32_t i : visible) {
        auto [draw, source] = drawOf(i);
        if (source == TerrainSurfaceSource::Baked) ++list.baked;
        if (source == TerrainSurfaceSource::Crossfade) ++list.crossfade;
        list.view.push_back(std::move(draw));
    }
    laps.lap("view draws");
    list.reflection.reserve(mirrored.size());
    for (const uint32_t i : mirrored) list.reflection.push_back(drawOf(i).first);
    laps.lap("reflection draws");
    if (bakeEnabled_) {
        list.atlases = bake_.extractNewAtlases();
        // A skipped frame keeps its jobs pending. Recording acknowledges them
        // through a completion token, never by writing into the live chunk.
        std::erase_if(bakeQueue_, [&](const std::pair<int, int>& place) {
            const uint32_t found = chunkIndex_.find(place.first, place.second);
            return found == TerrainChunkIndex::kNone || chunks[found].bakeComplete->load();
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
    for (const auto& [slot, pending] : pendingRows_) {
        list.rowSlots.push_back(slot);
        list.rows.push_back(pending.row);
    }
    return list;
}

void TerrainRenderer::uploadChunkRows(VkCommandBuffer cmd, uint32_t frameIndex, const DrawList& list) {
    if (list.rowCount == 0 || !chunkRows_.reserve(cmd, list.rowCount)) return;
    if (!chunkRows_.write(cmd, frameIndex, list.rowSlots, list.rows.data())) return;
    rowsUploaded_ = list.rowsExtraction;
    // This slot's set is rewritten after its fence, before any pass binds
    // it, and only when the table it names was replaced.
    if (chunkSetVersion_[frameIndex] == chunkRows_.version()) return;
    const VkDescriptorBufferInfo table{.buffer = chunkRows_.buffer(), .offset = 0, .range = chunkRows_.bytes()};
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = chunkSets_[frameIndex];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &table;
    vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
    chunkSetVersion_[frameIndex] = chunkRows_.version();
}

void TerrainRenderer::frameSubmitted() {
    if (rowsUploaded_ == 0) return;
    rowsSubmitted_.store(rowsUploaded_, std::memory_order_release);
    rowsUploaded_ = 0;
}

void TerrainRenderer::recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list) {
    const Recorded recorded = recordChunks(cmd, perFrameSet, list.view, list.wireframe);
    renderedTriangles_ = recorded.triangles;
    renderedDraws_ = recorded.draws;
    renderedChunks = static_cast<int>(list.view.size());
    culledChunks = list.culled;
    furthestDrawnSq_ = list.furthestSq;
    bakedDrawnChunks_ = list.baked;
    crossfadeDrawnChunks_ = list.crossfade;
}

void TerrainRenderer::recordReflection(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list) {
    recordChunks(cmd, perFrameSet, list.reflection, list.wireframe);
}

TerrainRenderer::Recorded TerrainRenderer::recordChunks(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                                        const std::vector<ChunkDraw>& draws, bool wireframe) {
    if (!pipeline || draws.empty()) return {};
    const uint32_t frameIndex = vkCtx->getCurrentFrame();
    // Without this frame's rows the ground is not drawn; the table said why.
    if (!chunkRowsReady(frameIndex)) return {};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      wireframe && wireframePipeline ? wireframePipeline : pipeline);
    const VkDescriptorSet sets[] = {perFrameSet, vkCtx->textureTable().set(), chunkSets_[frameIndex]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 3, sets, 0, nullptr);
    GPUPushConstants push{};
    push.model = glm::mat4(1.0f);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    Recorded recorded;
    ::VkBuffer boundVertices = VK_NULL_HANDLE, boundIndices = VK_NULL_HANDLE;
    LocalLightSet pushedLights = push.localLights;
    TerrainSurfacePush pushedSurface;
    bool surfacePushed = false;
    auto recordChunk = [&](const ChunkDraw& draw) {
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, kRowPushOffset, sizeof(draw.row),
                           &draw.row);
        if (draw.lights != pushedLights) {
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               offsetof(GPUPushConstants, localLights), sizeof(draw.lights), &draw.lights);
            pushedLights = draw.lights;
        }
        if (!surfacePushed || draw.surface.baked != pushedSurface.baked || draw.surface.relief != pushedSurface.relief) {
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               kSurfacePushOffset, sizeof(draw.surface), &draw.surface);
            pushedSurface = draw.surface;
            surfacePushed = true;
        }
        if (boundVertices != draw.geometry.vertices || boundIndices != draw.geometry.indices) {
            bindDraw(cmd, draw.geometry);
            boundVertices = draw.geometry.vertices;
            boundIndices = draw.geometry.indices;
        }
        for (const auto& range : draw.ranges) {
            if (!range.count) continue;
            vkCmdDrawIndexed(cmd, range.count, 1, draw.geometry.firstIndex + range.first,
                              draw.geometry.baseVertex, 0);
            recorded.triangles += range.count / 3;
            ++recorded.draws;
        }
    };
    // Chunks short of the distance discards first, through the pipeline whose
    // shader cannot discard; the rest after, through the one that can.
    // Terrain is opaque and depth-tested, so the order between chunks does
    // not change what is drawn.
    if (wireframe || !shortOfDiscardsPipeline) {
        for (const auto& draw : draws) recordChunk(draw);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shortOfDiscardsPipeline);
        for (const auto& draw : draws) {
            if (!draw.reachesDiscards) recordChunk(draw);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (const auto& draw : draws) {
            if (draw.reachesDiscards) recordChunk(draw);
        }
    }
    return recorded;
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
    if (!chunkRowsReady(frameIndex)) return;
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
    // A page refused before may fit now that every chunk is going.
    geometryPagesExhausted_ = false;
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
    if (chunk.row != TerrainChunkGPU::kNoRow) {
        pendingRows_.erase(chunk.row);
        freeRows_.push_back(chunk.row);
        chunk.row = TerrainChunkGPU::kNoRow;
    }
    // The chunk's ranges in its geometry page, given back once no frame in
    // flight draws them; until then nothing else is copied over them.
    std::shared_ptr<TerrainGeometryPage> page;
    if (chunk.geometryPage >= 0 && chunk.geometryPage < static_cast<int32_t>(geometryPages_.size())) {
        page = geometryPages_[chunk.geometryPage];
    }
    const auto baseVertex = static_cast<uint32_t>(chunk.baseVertex);
    const uint32_t vertexCount = chunk.vertexCount;
    const uint32_t firstIndex = chunk.firstIndex;
    // All of it, the coarser levels and skirts after the full mesh included.
    const uint32_t indexCount = chunk.allIndexCount;
    chunk.geometryPage = -1;

    // The textures are let go below: the shared cache keeps a tileset texture
    // for the next tile that wants it, and retires it only after the frames
    // that may still sample it. The tile's maps go with the tile
    // (releaseTileMaps).

    chunk.baseTexture.reset();
    for (TextureRef& t : chunk.layerTextures) t.reset();
    chunk.baseNormalMap.reset();
    for (TextureRef& t : chunk.layerNormalMaps) t.reset();
    chunk.maps = {};

    // A frame in flight may still draw them; the ranges come back after it.
    vkCtx->deferAfterAllFrameFences([page, baseVertex, vertexCount, firstIndex, indexCount]() {
        if (page) {
            page->vertexRanges.free(baseVertex, vertexCount);
            page->indexRanges.free(firstIndex, indexCount);
        }
    });
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
