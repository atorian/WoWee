#include "rendering/terrain_vertex.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/texture_upload.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/frustum.hpp"
#include "rendering/normal_map.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdlib>
#include <unordered_set>
#include <limits>
#include <cstring>

namespace wowee {
namespace rendering {

// Matches set 1 binding 7 in terrain.frag.glsl
struct TerrainParamsUBO {
    int32_t layerCount;
    int32_t hasLayer1;
    int32_t hasLayer2;
    int32_t hasLayer3;
};

// The fragment stage's push constants, after the vertex stage's
// GPUPushConstants. Matches `Surface` in terrain.frag.glsl.
struct TerrainSurfacePush {
    /// x: how steep the textures' relief is drawn, 0 for flat ground.
    glm::vec4 relief{0.0f};
};
constexpr uint32_t kSurfacePushOffset = sizeof(GPUPushConstants);

// The material set's bindings past the seven samplers and the params block:
// the base texture's relief, then each layer's.
constexpr uint32_t kFirstNormalMapBinding = 8;
constexpr uint32_t kMaterialBindingCount = kFirstNormalMapBinding + 4;
constexpr uint32_t kMaterialSamplerCount = 7 + 4;

/// The Sobel strength the ground's relief is derived with, half a wall's. The
/// ground is seen at a grazing angle across a whole road at once, where the
/// WMO renderer's two turned every grain of a cobble into a chiselled edge;
/// at one, Goldshire's road reads as stones and mortar at the default "Bump
/// strength", which scales the result in the shader.
constexpr float kTerrainNormalMapStrength = 1.0f;

TerrainRenderer::TerrainRenderer() = default;

TerrainRenderer::~TerrainRenderer() {
    shutdown();
}

/// Builds the fill pipeline and its wireframe derivative from a loaded shader
/// pair.
///
/// initialize() and recreatePipelines() both need exactly these two in exactly
/// these states, and each described both for itself. Answers whether the fill
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

    // --- Create material descriptor set layout (set 1) ---
    // bindings 0-6: combined image samplers (base + 3 layer + 3 alpha)
    // binding 7: uniform buffer (TerrainParams)
    // bindings 8-11: combined image samplers (base + 3 layer normal maps)
    std::vector<VkDescriptorSetLayoutBinding> materialBindings(kMaterialBindingCount);
    for (uint32_t i = 0; i < kMaterialBindingCount; i++) {
        materialBindings[i] = {};
        materialBindings[i].binding = i;
        materialBindings[i].descriptorType = i == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        materialBindings[i].descriptorCount = 1;
        materialBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    materialSetLayout = createDescriptorSetLayout(device, materialBindings);
    if (!materialSetLayout) {
        LOG_ERROR("TerrainRenderer: failed to create material set layout");
        return false;
    }

    // --- Create descriptor pool ---
    VkDescriptorPoolSize poolSizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_MATERIAL_SETS * kMaterialSamplerCount },
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_MATERIAL_SETS },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = MAX_MATERIAL_SETS;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &materialDescPool) != VK_SUCCESS) {
        LOG_ERROR("TerrainRenderer: failed to create descriptor pool");
        return false;
    }

    // --- Create pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(GPUPushConstants);
    VkPushConstantRange surfaceRange{};
    surfaceRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    surfaceRange.offset = kSurfacePushOffset;
    surfaceRange.size = sizeof(TerrainSurfacePush);

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, materialSetLayout };
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
    const uint8_t opaqueAlpha = 255;
    opaqueAlphaTexture = adoptPixels(*vkCtx, TextureForm::Owned, &opaqueAlpha, 1, 1, VK_FORMAT_R8_UNORM,
                                     false, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    // Straight up, for a texture whose relief could not be had.
    const uint8_t flatNormal[4] = {128, 128, 255, 128};
    flatNormalTexture = adoptPixels(*vkCtx, TextureForm::Owned, flatNormal, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                    false, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    // The geometry pages are made as tiles arrive (placeInGeometryPage).

    LOG_INFO("Terrain renderer initialized (Vulkan)");
    return true;
}

void TerrainRenderer::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    // Destroy old pipelines (keep layouts)
    destroy(device, pipeline);
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

    vkDeviceWaitIdle(device);

    clear();
    // clear() defers chunk destruction, and no further frames will run to drain
    // the queue - so run it now, while the descriptor pools those lambdas free
    // sets from are still alive. Without this every resident chunk's vertex and
    // index buffers outlived the device: twenty thousand of each.
    vkCtx->flushDeferredCleanup();

    loggedTextureLoadFails_.clear();
    loggedNormalMapFails_.clear();
    whiteTexture.reset();
    opaqueAlphaTexture.reset();
    flatNormalTexture.reset();

    destroy(device, pipeline);
    destroy(device, wireframePipeline);
    destroy(device, pipelineLayout);
    destroy(device, materialDescPool);
    destroy(device, materialSetLayout);

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

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            const auto& chunk = mesh.getChunk(x, y);
            if (!chunk.isValid()) continue;

            TerrainChunkGPU gpuChunk = uploadChunk(chunk);
            if (!gpuChunk.isValid()) {
                LOG_WARNING("Failed to upload chunk [", x, ",", y, "]");
                continue;
            }

            calculateBoundingSphere(gpuChunk, chunk);

            // Load textures for this chunk
            bindChunkTextures(gpuChunk, chunk, texturePaths, tileX, tileY, x, y);

            gpuChunk.tileX = tileX;
            gpuChunk.tileY = tileY;

            // Create per-chunk params UBO
            // A failed allocation here is pressure rather than corruption, but
            // this path has no way to come back for the chunk, so it skips it.
            if (!createChunkParamsUBO(gpuChunk)) {
                LOG_WARNING("Terrain chunk UBO allocation failed - skipping chunk");
                destroyChunkGPU(gpuChunk);
                continue;
            }

            gpuChunk.materialSet = allocateMaterialSet();
            if (!gpuChunk.materialSet) {
                destroyChunkGPU(gpuChunk);
                continue;
            }
            if (!writeMaterialDescriptors(gpuChunk.materialSet, gpuChunk)) {
                destroyChunkGPU(gpuChunk);
                continue;
            }

            registerRtChunk(gpuChunk, chunk);
            chunks.push_back(std::move(gpuChunk));
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

    int uploaded = 0;
    while (chunkIndex < 256 && uploaded < maxChunksPerCall) {
        int cy = chunkIndex / 16;
        int cx = chunkIndex % 16;
        chunkIndex++;

        const auto& chunk = mesh.getChunk(cx, cy);
        if (!chunk.isValid()) continue;

        TerrainChunkGPU gpuChunk = uploadChunk(chunk);
        if (!gpuChunk.isValid()) continue;

        calculateBoundingSphere(gpuChunk, chunk);

        bindChunkTextures(gpuChunk, chunk, texturePaths, tileX, tileY, cx, cy);

        gpuChunk.tileX = tileX;
        gpuChunk.tileY = tileY;

        if (!createChunkParamsUBO(gpuChunk)) {
            LOG_WARNING("Terrain[", tileX, ",", tileY, "] chunk UBO allocation failed"
                        " - retrying next frame");
            destroyChunkGPU(gpuChunk);
            chunkIndex--;
            break;
        }

        gpuChunk.materialSet = allocateMaterialSet();
        if (!gpuChunk.materialSet) {
            destroyChunkGPU(gpuChunk);
            // Give the chunk back rather than dropping it. Both failures above
            // are pressure, not corruption: the descriptor pool is shared by
            // every resident tile and its sets come back only through
            // deferAfterAllFrameFences, so a tile arriving while an unloaded
            // one is still in flight can find the pool momentarily full. A
            // dropped chunk was never retried - the tile counted as loaded
            // with a hole in it, and the hole stayed for the rest of the
            // session. Stepping the index back and leaving means the caller
            // sees "not finished" and comes back once the frees have landed.
            chunkIndex--;
            break;
        }
        if (!writeMaterialDescriptors(gpuChunk.materialSet, gpuChunk)) {
            // Not the retry above: that one is descriptor-pool pressure, which
            // passes. This is the fallback textures being unsampleable, which
            // does not, so the chunk is dropped rather than asked for again.
            destroyChunkGPU(gpuChunk);
            continue;
        }

        registerRtChunk(gpuChunk, chunk);
        chunks.push_back(std::move(gpuChunk));
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

        // A layer with no alpha map covers everything under it.
        gpuChunk.alphaTextures[li] = layer.alphaData.empty() ? opaqueAlphaTexture
                                                             : createAlphaTexture(layer.alphaData);
        gpuChunk.layerCount = static_cast<int>(i);
    }
}

bool TerrainRenderer::createChunkParamsUBO(TerrainChunkGPU& gpuChunk) {
    TerrainParamsUBO params{};
    params.layerCount = gpuChunk.layerCount;
    params.hasLayer1 = gpuChunk.layerCount >= 1 ? 1 : 0;
    params.hasLayer2 = gpuChunk.layerCount >= 2 ? 1 : 0;
    params.hasLayer3 = gpuChunk.layerCount >= 3 ? 1 : 0;

    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = sizeof(TerrainParamsUBO);
    bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapInfo{};
    // Check the return value - a null UBO handle would leave the GPU reading
    // from an invalid descriptor, crashing the driver under memory pressure
    // rather than losing one chunk.
    if (vmaCreateBuffer(vkCtx->getAllocator(), &bufCI, &allocCI,
                        &gpuChunk.paramsUBO, &gpuChunk.paramsAlloc, &mapInfo) != VK_SUCCESS) {
        return false;
    }
    if (mapInfo.pMappedData) {
        std::memcpy(mapInfo.pMappedData, &params, sizeof(params));
    }
    return true;
}

TerrainChunkGPU TerrainRenderer::uploadChunk(const pipeline::ChunkMesh& chunk) {
    TerrainChunkGPU gpuChunk;

    gpuChunk.worldX = chunk.worldX;
    gpuChunk.worldY = chunk.worldY;
    gpuChunk.worldZ = chunk.worldZ;
    gpuChunk.indexCount = static_cast<uint32_t>(chunk.indices.size());
    gpuChunk.vertexCount = static_cast<uint32_t>(chunk.vertices.size());

    if (placeInGeometryPage(gpuChunk, chunk)) return gpuChunk;

    // No page could be made: the chunk is drawn from buffers of its own,
    // which costs it a rebind in both passes but keeps the ground there.
    VkDeviceSize vbSize = chunk.vertices.size() * sizeof(pipeline::TerrainVertex);
    AllocatedBuffer vb = uploadBuffer(*vkCtx, chunk.vertices.data(), vbSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpuChunk.vertexBuffer = vb.buffer;
    gpuChunk.vertexAlloc = vb.allocation;

    VkDeviceSize ibSize = chunk.indices.size() * sizeof(pipeline::TerrainIndex);
    AllocatedBuffer ib = uploadBuffer(*vkCtx, chunk.indices.data(), ibSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    gpuChunk.indexBuffer = ib.buffer;
    gpuChunk.indexAlloc = ib.allocation;
    if (gpuChunk.isValid()) ++ownBufferChunks_;

    return gpuChunk;
}

bool TerrainRenderer::placeInGeometryPage(TerrainChunkGPU& gpuChunk, const pipeline::ChunkMesh& chunk) {
    const auto vertexCount = static_cast<uint32_t>(chunk.vertices.size());
    const auto indexCount = static_cast<uint32_t>(chunk.indices.size());
    if (vertexCount == 0 || indexCount == 0 ||
        vertexCount > kPageVertices || indexCount > kPageIndices) {
        return false;
    }

    // A page with room, else a new one in the first empty slot.
    int32_t slot = -1;
    std::optional<uint32_t> firstVertex;
    std::optional<uint32_t> firstIndex;
    for (size_t i = 0; i < geometryPages_.size() && slot < 0; ++i) {
        TerrainGeometryPage* page = geometryPages_[i].get();
        if (!page || page->vertexRanges.largestFree() < vertexCount ||
            page->indexRanges.largestFree() < indexCount) {
            continue;
        }
        firstVertex = page->vertexRanges.allocate(vertexCount);
        firstIndex = page->indexRanges.allocate(indexCount);
        slot = static_cast<int32_t>(i);
    }
    if (slot < 0 && geometryPagesExhausted_) return false;
    if (slot < 0) {
        auto page = std::make_shared<TerrainGeometryPage>();
        VmaAllocator allocator = vkCtx->getAllocator();
        page->vertices = createBuffer(allocator,
            static_cast<VkDeviceSize>(kPageVertices) * sizeof(pipeline::TerrainVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);
        page->indices = createBuffer(allocator,
            static_cast<VkDeviceSize>(kPageIndices) * sizeof(pipeline::TerrainIndex),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);
        if (!page->vertices.buffer || !page->indices.buffer) {
            if (page->vertices.buffer) destroyBuffer(allocator, page->vertices);
            if (page->indices.buffer) destroyBuffer(allocator, page->indices);
            geometryPagesExhausted_ = true;
            LOG_WARNING("Terrain: no geometry page could be allocated (",
                        geometryStats().pages, " pages resident) - chunks get buffers of"
                        " their own until a page is let go, at a rebind each per pass");
            return false;
        }
        page->vertexRanges = RangeAllocator(kPageVertices);
        page->indexRanges = RangeAllocator(kPageIndices);
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
        LOG_INFO("Terrain: geometry page ", slot, " made (", geometryStats().pages,
                 " resident, ", kTilesPerGeometryPage, " tiles each)");
    }

    TerrainGeometryPage& page = *geometryPages_[slot];
    const bool copied =
        uploadIntoBuffer(*vkCtx, chunk.vertices.data(),
                         static_cast<VkDeviceSize>(vertexCount) * sizeof(pipeline::TerrainVertex),
                         page.vertices.buffer,
                         static_cast<VkDeviceSize>(*firstVertex) * sizeof(pipeline::TerrainVertex)) &&
        uploadIntoBuffer(*vkCtx, chunk.indices.data(),
                         static_cast<VkDeviceSize>(indexCount) * sizeof(pipeline::TerrainIndex),
                         page.indices.buffer,
                         static_cast<VkDeviceSize>(*firstIndex) * sizeof(pipeline::TerrainIndex));
    if (!copied) {
        // Nothing has drawn these ranges, so they come back at once.
        page.vertexRanges.free(*firstVertex, vertexCount);
        page.indexRanges.free(*firstIndex, indexCount);
        LOG_WARNING("Terrain: a chunk's geometry could not be staged into page ", slot,
                    " - it gets buffers of its own");
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
        if (!spareKept) {
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
    ownBufferChunks_ = 0;
    geometryPagesExhausted_ = false;
}

void TerrainRenderer::bindGeometryPage(VkCommandBuffer cmd, const TerrainGeometryPage& page) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &page.vertices.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, page.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
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
    }
    stats.ownBufferChunks = ownBufferChunks_;
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

TextureRef TerrainRenderer::createAlphaTexture(const std::vector<uint8_t>& alphaData) {
    if (alphaData.empty()) return opaqueAlphaTexture;

    std::vector<uint8_t> expanded;
    const uint8_t* src = alphaData.data();
    if (alphaData.size() < 4096) {
        expanded.assign(4096, 255);
        std::copy(alphaData.begin(), alphaData.end(), expanded.begin());
        src = expanded.data();
    }

    // One per chunk layer and the chunk's alone: counted against the shared
    // budget, gone with the chunk. Never refused, since a chunk without its
    // alpha map draws its top layer over everything below.
    TextureRef alpha = adoptPixels(*vkCtx, TextureForm::Owned, src, 64, 64, VK_FORMAT_R8_UNORM, false,
                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    return alpha ? alpha : opaqueAlphaTexture;
}

VkDescriptorSet TerrainRenderer::allocateMaterialSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx->getDevice(), &allocInfo, &set) != VK_SUCCESS) {
        static uint64_t failCount = 0;
        ++failCount;
        if (failCount <= 8 || (failCount % 256) == 0) {
            LOG_WARNING("TerrainRenderer: failed to allocate material descriptor set (count=", failCount, ")");
        }
        return VK_NULL_HANDLE;
    }
    return set;
}

bool TerrainRenderer::writeMaterialDescriptors(VkDescriptorSet set, const TerrainChunkGPU& chunk) {
    VkTexture* white = whiteTexture.get();
    VkTexture* opaque = opaqueAlphaTexture.get();

    // Valid, not merely non-null. descriptorInfo() returns the texture's
    // handles as they are, so one whose upload or view creation failed writes
    // VK_NULL_HANDLE into a live descriptor and declares
    // SHADER_READ_ONLY_OPTIMAL over it. Sampling that is undefined behaviour
    // and reaches an NVIDIA driver as a graphics engine exception and a lost
    // device rather than as anything this client can catch. See #123.
    //
    // A chunk texture failing is ordinary - it is what the cache does under
    // memory pressure - and the 1x1 fallbacks are what that case is for.
    const auto sampleable = [](VkTexture* t) { return t && t->isValid(); };
    const auto pick = [&](VkTexture* wanted, VkTexture* fallback) {
        return sampleable(wanted) ? wanted : fallback;
    };
    if (!sampleable(white) || !sampleable(opaque)) {
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
    VkTexture* flat = pick(flatNormalTexture.get(), white);

    // Indexed by binding; 7 is the params block.
    VkDescriptorImageInfo imageInfos[kMaterialBindingCount] = {};
    imageInfos[0] = pick(chunk.baseTexture.get(), white)->descriptorInfo();
    imageInfos[kFirstNormalMapBinding] = pick(chunk.baseNormalMap.get(), flat)->descriptorInfo();
    for (uint32_t i = 0; i < 3; i++) {
        imageInfos[1 + i] = pick(chunk.layerTextures[i].get(), white)->descriptorInfo();
        imageInfos[4 + i] = pick(chunk.alphaTextures[i].get(), opaque)->descriptorInfo();
        imageInfos[kFirstNormalMapBinding + 1 + i] = pick(chunk.layerNormalMaps[i].get(), flat)->descriptorInfo();
    }

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = chunk.paramsUBO;
    bufInfo.offset = 0;
    bufInfo.range = sizeof(TerrainParamsUBO);

    VkWriteDescriptorSet writes[kMaterialBindingCount] = {};
    for (uint32_t i = 0; i < kMaterialBindingCount; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i == 7) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[i].pBufferInfo = &bufInfo;
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imageInfos[i];
        }
    }

    vkUpdateDescriptorSets(vkCtx->getDevice(), kMaterialBindingCount, writes, 0, nullptr);
    return true;
}

void TerrainRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera) {
    if (chunks.empty() || !pipeline) {
        static int emptyLog = 0;
        if (++emptyLog <= 3)
            LOG_WARNING("TerrainRenderer::render: chunks=", chunks.size(), " pipeline=", (pipeline != VK_NULL_HANDLE));
        return;
    }

    // One-time diagnostic: log chunk nearest to camera
    static bool loggedDiag = false;
    if (!loggedDiag && !chunks.empty()) {
        loggedDiag = true;
        glm::vec3 cam = camera.getPosition();
        // Find chunk nearest to camera
        const TerrainChunkGPU* nearest = nullptr;
        float nearestDist = std::numeric_limits<float>::max();
        for (const auto& ch : chunks) {
            float dx = ch.boundingSphereCenter.x - cam.x;
            float dy = ch.boundingSphereCenter.y - cam.y;
            float dz = ch.boundingSphereCenter.z - cam.z;
            float d = dx*dx + dy*dy + dz*dz;
            if (d < nearestDist) { nearestDist = d; nearest = &ch; }
        }
        if (nearest) {
            float d2d = std::sqrt((nearest->boundingSphereCenter.x-cam.x)*(nearest->boundingSphereCenter.x-cam.x) +
                                  (nearest->boundingSphereCenter.y-cam.y)*(nearest->boundingSphereCenter.y-cam.y));
            LOG_INFO("Terrain diag: chunks=", chunks.size(),
                     " cam=(", cam.x, ",", cam.y, ",", cam.z, ")",
                     " nearest_center=(", nearest->boundingSphereCenter.x, ",", nearest->boundingSphereCenter.y, ",", nearest->boundingSphereCenter.z, ")",
                     " dist2d=", d2d, " dist3d=", std::sqrt(nearestDist),
                     " radius=", nearest->boundingSphereRadius,
                     " matSet=", (nearest->materialSet != VK_NULL_HANDLE ? "ok" : "NULL"));
        }
    }

    VkPipeline activePipeline = (wireframe && wireframePipeline) ? wireframePipeline : pipeline;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                             0, 1, &perFrameSet, 0, nullptr);

    GPUPushConstants push{};
    push.model = glm::mat4(1.0f);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                        0, sizeof(GPUPushConstants), &push);
    LocalLightSet pushedLights = push.localLights;
    TerrainSurfacePush surface{};
    surface.relief.x = normalMappingEnabled_ ? normalMapStrength_ : 0.0f;
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       kSurfacePushOffset, sizeof(TerrainSurfacePush), &surface);

    Frustum frustum;
    if (frustumCullingEnabled) {
        glm::mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();
        frustum.extractFromMatrix(viewProj);
    }

    glm::vec3 camPos = camera.getPosition();

    renderedChunks = 0;
    culledChunks = 0;
    furthestDrawnSq_ = 0.0f;

    // The geometry page bound now; a chunk in another page, or one with
    // buffers of its own, binds anew. Chunks of a tile are listed together and
    // a tile's go in one page, so this is a bind per page, not per chunk.
    constexpr int32_t kNothingBound = -2;
    int32_t boundPage = kNothingBound;

    for (const auto& chunk : chunks) {
        if (!chunk.isValid() || !chunk.materialSet) continue;

        float dx = chunk.boundingSphereCenter.x - camPos.x;
        float dy = chunk.boundingSphereCenter.y - camPos.y;
        float distSq = dx * dx + dy * dy;
        // Keep any chunk intersecting the common range. The fragment shader
        // clips at the exact limit; testing only the center removes foothills early.
        const float chunkLimit = maxViewDistance_ + chunk.boundingSphereRadius;
        if (distSq > chunkLimit * chunkLimit) {
            culledChunks++;
            continue;
        }

        if (frustumCullingEnabled && !isChunkVisible(chunk, frustum)) {
            culledChunks++;
            continue;
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                 1, 1, &chunk.materialSet, 0, nullptr);

        // The lights nearest the chunk (local_lights.hpp).
        const LocalLightSet chunkLights = frameLights_ && !frameLights_->empty()
            ? pickLocalLights(*frameLights_, chunk.boundingSphereCenter,
                              chunk.boundingSphereRadius)
            : kNoLocalLights;
        if (chunkLights != pushedLights) {
            pushedLights = chunkLights;
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               offsetof(GPUPushConstants, localLights), sizeof(LocalLightSet), &pushedLights);
        }

        if (chunk.geometryPage >= 0) {
            // Rebound after a chunk with buffers of its own, too: its buffers
            // stay bound for whatever comes next, and a page's offsets drawn
            // against one chunk's buffer - firstIndex 6,290,784 into 3,072
            // bytes - is what once took the GPU down.
            if (boundPage != chunk.geometryPage) {
                bindGeometryPage(cmd, *geometryPages_[chunk.geometryPage]);
                boundPage = chunk.geometryPage;
            }
            vkCmdDrawIndexed(cmd, chunk.indexCount, 1, chunk.firstIndex, chunk.baseVertex, 0);
        } else {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &chunk.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, chunk.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, chunk.indexCount, 1, 0, 0, 0);
            boundPage = kNothingBound;
        }
        renderedChunks++;
        if (distSq > furthestDrawnSq_) furthestDrawnSq_ = distSq;
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
    pc.size = sizeof(ShadowPush);  // one combined matrix, plus the sway slot
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

    // The shadow shader is shared with the skinned renderers, so it declares
    // bone inputs terrain has none of; kTerrainShadowVertexAttributes says
    // where they point and why.
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

void TerrainRenderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                                    const glm::vec3& shadowCenter, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (chunks.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
        0, 1, &shadowParams_.set, 0, nullptr);

    // Identity model matrix - terrain vertices are already in world space
    // Terrain is already in world space, so the model matrix it used to push
    // was the identity: the combined matrix is the light-space one.
    ShadowPush push{.lightSpaceModel = lightSpaceMatrix};
    vkCmdPushConstants(cmd, shadowPipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(ShadowPush), &push);

    // Bound per geometry page, as in the main pass.
    constexpr int32_t kNothingBound = -2;
    int32_t boundPage = kNothingBound;

    // Culled against the map's own footprint, not a sphere round the player.
    // The map is a box reaching hundreds of yards toward the sun, and a ridge
    // that far off in that direction is exactly what shades the ground and the
    // air round the player - the sphere left it out, so the sun lit the mist
    // straight through it. A chunk is kept if its centre lands on the map
    // across the light, at any depth along it; the orthographic scale of the
    // matrix is what turns the chunk's radius into the same units.
    //
    // Bounded in depth as well. Beyond the player, on the side away from the
    // sun, terrain shades nothing the camera is near - and left in, a low sun
    // turned the footprint into a strip the length of the loaded world, and
    // the shadow pass drew thousands of chunks a frame.
    const glm::vec4 centreLs = lightSpaceMatrix * glm::vec4(shadowCenter, 1.0f);
    const float scaleZ = glm::length(glm::vec3(lightSpaceMatrix[0][2], lightSpaceMatrix[1][2],
                                               lightSpaceMatrix[2][2]));
    const float scaleX = glm::length(glm::vec3(lightSpaceMatrix[0][0], lightSpaceMatrix[1][0],
                                               lightSpaceMatrix[2][0]));
    const float scaleY = glm::length(glm::vec3(lightSpaceMatrix[0][1], lightSpaceMatrix[1][1],
                                               lightSpaceMatrix[2][1]));

    uint32_t shadowChunksDrawn = 0;
    for (const auto& chunk : chunks) {
        if (!chunk.isValid()) continue;

        const glm::vec4 ls = lightSpaceMatrix * glm::vec4(chunk.boundingSphereCenter, 1.0f);
        if (std::abs(ls.x) > 1.0f + chunk.boundingSphereRadius * scaleX ||
            std::abs(ls.y) > 1.0f + chunk.boundingSphereRadius * scaleY) {
            continue;
        }
        // Depth grows away from the light in either depth convention.
        if (ls.z > centreLs.z + (shadowRadius + chunk.boundingSphereRadius) * scaleZ) continue;
        ++shadowChunksDrawn;

        if (chunk.geometryPage >= 0) {
            if (boundPage != chunk.geometryPage) {
                bindGeometryPage(cmd, *geometryPages_[chunk.geometryPage]);
                boundPage = chunk.geometryPage;
            }
            vkCmdDrawIndexed(cmd, chunk.indexCount, 1, chunk.firstIndex, chunk.baseVertex, 0);
        } else {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &chunk.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, chunk.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, chunk.indexCount, 1, 0, 0, 0);
            boundPage = kNothingBound;
        }
    }
    // How many chunks cast, said when it moves by a quarter: the count is the
    // cost of this pass, and the cull that decides it is not obvious.
    static uint32_t lastReported = 0;
    if (shadowChunksDrawn > lastReported + lastReported / 4 + 16 ||
        shadowChunksDrawn + lastReported / 4 + 16 < lastReported) {
        LOG_WARNING("Terrain shadow pass: ", shadowChunksDrawn, " of ", chunks.size(),
                    " chunks cast");
        lastReported = shadowChunksDrawn;
    }
}

void TerrainRenderer::removeTile(int tileX, int tileY) {
    int removed = 0;
    auto it = chunks.begin();
    while (it != chunks.end()) {
        if (it->tileX == tileX && it->tileY == tileY) {
            destroyChunkGPU(*it);
            it = chunks.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        LOG_DEBUG("Removed ", removed, " terrain chunks for tile [", tileX, ",", tileY, "]");
    }
}

void TerrainRenderer::clear() {
    if (!vkCtx) return;

    for (auto& chunk : chunks) {
        destroyChunkGPU(chunk);
    }
    chunks.clear();
    renderedChunks = 0;
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

    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    // These resources may still be referenced by in-flight command buffers from
    // previous frames. Defer actual destruction until this frame slot is safe.
    ::VkBuffer vertexBuffer = chunk.vertexBuffer;
    VmaAllocation vertexAlloc = chunk.vertexAlloc;
    ::VkBuffer indexBuffer = chunk.indexBuffer;
    VmaAllocation indexAlloc = chunk.indexAlloc;
    ::VkBuffer paramsUBO = chunk.paramsUBO;
    VmaAllocation paramsAlloc = chunk.paramsAlloc;
    VkDescriptorPool pool = materialDescPool;
    VkDescriptorSet materialSet = chunk.materialSet;
    // The chunk's ranges in its geometry page, given back once no frame in
    // flight draws them; until then nothing else is copied over them.
    std::shared_ptr<TerrainGeometryPage> page;
    if (chunk.geometryPage >= 0 && chunk.geometryPage < static_cast<int32_t>(geometryPages_.size())) {
        page = geometryPages_[chunk.geometryPage];
    }
    const auto baseVertex = static_cast<uint32_t>(chunk.baseVertex);
    const uint32_t vertexCount = chunk.vertexCount;
    const uint32_t firstIndex = chunk.firstIndex;
    const uint32_t indexCount = chunk.indexCount;
    if (chunk.geometryPage < 0 && chunk.vertexBuffer && ownBufferChunks_ > 0) --ownBufferChunks_;
    chunk.geometryPage = -1;

    // The textures are let go below: the shared cache keeps a tileset texture
    // for the next tile that wants it and retires the chunk's own alpha maps,
    // either way only after the frames that may still sample them.

    chunk.vertexBuffer = VK_NULL_HANDLE;
    chunk.vertexAlloc = VK_NULL_HANDLE;
    chunk.indexBuffer = VK_NULL_HANDLE;
    chunk.indexAlloc = VK_NULL_HANDLE;
    chunk.paramsUBO = VK_NULL_HANDLE;
    chunk.paramsAlloc = VK_NULL_HANDLE;
    chunk.materialSet = VK_NULL_HANDLE;
    chunk.baseTexture.reset();
    for (TextureRef& t : chunk.layerTextures) t.reset();
    for (TextureRef& t : chunk.alphaTextures) t.reset();
    chunk.baseNormalMap.reset();
    for (TextureRef& t : chunk.layerNormalMaps) t.reset();

    vkCtx->deferAfterAllFrameFences([device, allocator, vertexBuffer, vertexAlloc, indexBuffer, indexAlloc,
                                     paramsUBO, paramsAlloc, pool, materialSet,
                                     page, baseVertex, vertexCount, firstIndex, indexCount]() {
        if (page) {
            page->vertexRanges.free(baseVertex, vertexCount);
            page->indexRanges.free(firstIndex, indexCount);
        }
        if (vertexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = vertexBuffer; ab.allocation = vertexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (indexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = indexBuffer; ab.allocation = indexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (paramsUBO) {
            AllocatedBuffer ab{}; ab.buffer = paramsUBO; ab.allocation = paramsAlloc;
            destroyBuffer(allocator, ab);
        }
        if (materialSet && pool) {
            VkDescriptorSet set = materialSet;
            vkFreeDescriptorSets(device, pool, 1, &set);
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

bool TerrainRenderer::isChunkVisible(const TerrainChunkGPU& chunk, const Frustum& frustum) {
    return frustum.intersectsSphere(chunk.boundingSphereCenter, chunk.boundingSphereRadius);
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
