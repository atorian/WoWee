#include "rendering/terrain_bake.hpp"

#include "core/logger.hpp"
#include "rendering/terrain_vertex.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace wowee {
namespace rendering {

namespace {

constexpr VkFormat kAtlasFormat = VK_FORMAT_R8G8B8A8_UNORM;

VkImageMemoryBarrier2 imageBarrier(VkImage image, uint32_t baseMip, uint32_t mipCount,
                                   VkImageLayout from, VkImageLayout to,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = baseMip,
                          .levelCount = mipCount, .baseArrayLayer = 0, .layerCount = 1};
    return b;
}

void barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* list, uint32_t count) {
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = list;
    cmdPipelineBarrier2(cmd, dep);
}

void destroyAtlasImages(VkDevice device, VmaAllocator allocator, TerrainBake::Atlas& atlas) {
    for (VkImageView* view : {&atlas.colorTarget, &atlas.reliefTarget}) {
        if (*view) vkDestroyImageView(device, *view, nullptr);
        *view = VK_NULL_HANDLE;
    }
    destroyImage(device, allocator, atlas.color);
    destroyImage(device, allocator, atlas.relief);
}

VkImageView levelZeroView(VkDevice device, VkImage image) {
    VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = kAtlasFormat;
    info.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                             .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

}  // namespace

TerrainBake::~TerrainBake() { shutdown(); }

bool TerrainBake::initialize(VkContext& ctx, VkDescriptorSetLayout materialSetLayout) {
    ctx_ = &ctx;
    mipLevels_ = static_cast<uint32_t>(std::bit_width(kTerrainBakeAtlasSize));
    if (!ctx.useDynamicRendering()) {
        LOG_WARNING("Terrain bake: needs dynamic rendering, which is off here; the far ground "
                    "is drawn from its layers however far it is");
        return false;
    }
    VkDevice device = ctx.getDevice();

    pipelineLayout_ = createPipelineLayout(device, {materialSetLayout}, {});
    if (!pipelineLayout_) {
        LOG_ERROR("Terrain bake: could not create the pipeline layout; the far ground is drawn "
                  "from its layers however far it is");
        return false;
    }

    VkShaderModule vert, frag;
    if (!vert.loadFromFile(device, "assets/shaders/terrain_bake.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/terrain_bake.frag.spv")) {
        LOG_ERROR("Terrain bake: terrain_bake shaders missing; the far ground is drawn from its "
                  "layers however far it is");
        vert.destroy();
        frag.destroy();
        destroy(device, pipelineLayout_);
        return false;
    }
    // The bake reads the texture coordinate and the alpha-map coordinate.
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const auto& attribute : kTerrainVertexAttributes) {
        if (attribute.location == 2 || attribute.location == 3) attributes.push_back(toVkAttribute(attribute));
    }
    pipeline_ = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({perVertexBinding(sizeof(pipeline::TerrainVertex))}, attributes)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachments({PipelineBuilder::blendDisabled(), PipelineBuilder::blendDisabled()})
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(pipelineLayout_)
        .setRenderingFormats({kAtlasFormat, kAtlasFormat})
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, ctx.getPipelineCache());
    vert.destroy();
    frag.destroy();
    if (!pipeline_) {
        LOG_ERROR("Terrain bake: could not create the pipeline; the far ground is drawn from its "
                  "layers however far it is");
        destroy(device, pipelineLayout_);
        return false;
    }

    // As the layers are sampled (giveCachedSampler): linear, every mip,
    // anisotropic as far as the device goes - so at the same distance and the
    // same slant the atlas is filtered the way the layers would have been.
    // Clamped, not repeated: past a tile's edge is another tile's atlas.
    VkSamplerCreateInfo s{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = VK_FILTER_LINEAR;
    s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.anisotropyEnable = VK_TRUE;
    s.maxAnisotropy = 16.0f;
    s.minLod = 0.0f;
    s.maxLod = static_cast<float>(mipLevels_ - 1);
    sampler_ = ctx.getOrCreateSampler(s);
    if (!sampler_) {
        LOG_ERROR("Terrain bake: could not create the atlas sampler; the far ground is drawn "
                  "from its layers however far it is");
        destroy(device, pipeline_);
        destroy(device, pipelineLayout_);
        return false;
    }
    return true;
}

void TerrainBake::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    for (auto& [key, atlas] : atlases_) destroyAtlas(*atlas);
    atlases_.clear();
    destroy(device, pipeline_);
    destroy(device, pipelineLayout_);
    sampler_ = VK_NULL_HANDLE;  // the context's sampler cache owns it
    ctx_ = nullptr;
}

void TerrainBake::destroyAtlas(Atlas& atlas) {
    destroyAtlasImages(ctx_->getDevice(), ctx_->getAllocator(), atlas);
}

TerrainBake::Atlas* TerrainBake::atlas(int tileX, int tileY) {
    if (!enabled()) return nullptr;
    const uint64_t key = tileKey(tileX, tileY);
    if (auto it = atlases_.find(key); it != atlases_.end()) return it->second.get();

    VkDevice device = ctx_->getDevice();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto made = std::make_shared<Atlas>();
    made->color = createImage(device, ctx_->getAllocator(), kTerrainBakeAtlasSize, kTerrainBakeAtlasSize,
                              kAtlasFormat, usage, VK_SAMPLE_COUNT_1_BIT, mipLevels_);
    made->relief = createImage(device, ctx_->getAllocator(), kTerrainBakeAtlasSize, kTerrainBakeAtlasSize,
                               kAtlasFormat, usage, VK_SAMPLE_COUNT_1_BIT, mipLevels_);
    if (made->color.imageView && made->relief.imageView) {
        made->colorTarget = levelZeroView(device, made->color.image);
        made->reliefTarget = levelZeroView(device, made->relief.image);
    }
    if (!made->colorTarget || !made->reliefTarget) {
        if (!atlasFailureSaid_) {
            atlasFailureSaid_ = true;
            LOG_WARNING("Terrain bake: no atlas for tile [", tileX, ",", tileY,
                        "] (createImage said why); its far ground is drawn from its layers, and "
                        "so is any other tile's that fails the same way");
        }
        destroyAtlas(*made);
        return nullptr;
    }
    Atlas* atlas = made.get();
    atlases_.emplace(key, std::move(made));
    return atlas;
}

void TerrainBake::releaseTile(int tileX, int tileY) {
    auto it = atlases_.find(tileKey(tileX, tileY));
    if (it == atlases_.end()) return;
    std::shared_ptr<Atlas> retired(std::move(it->second));
    atlases_.erase(it);
    // The chunks that named it in their material sets may still be drawn by
    // the frames in flight.
    const VkDevice device = ctx_->getDevice();
    const VmaAllocator allocator = ctx_->getAllocator();
    ctx_->deferAfterAllFrameFences([device, allocator, retired] {
        destroyAtlasImages(device, allocator, *retired);
    });
}

void TerrainBake::releaseAll() {
    std::vector<uint64_t> keys;
    keys.reserve(atlases_.size());
    for (const auto& [key, atlas] : atlases_) keys.push_back(key);
    for (const uint64_t key : keys) {
        releaseTile(static_cast<int>(static_cast<int32_t>(key >> 32)),
                    static_cast<int>(static_cast<int32_t>(key & 0xffffffffu)));
    }
}

uint64_t TerrainBake::atlasBytes() const {
    // Both images, every mip: a third again over the first.
    const uint64_t first = uint64_t(kTerrainBakeAtlasSize) * kTerrainBakeAtlasSize * 4;
    return atlases_.size() * 2 * (first + first / 3);
}

std::vector<std::shared_ptr<TerrainBake::Atlas>> TerrainBake::extractNewAtlases() const {
    std::vector<std::shared_ptr<Atlas>> result;
    for (const auto& [key, atlas] : atlases_) if (!atlas->ready.load()) result.push_back(atlas);
    return result;
}

void TerrainBake::prepareNewAtlases(VkCommandBuffer cmd, std::span<const std::shared_ptr<Atlas>> atlases) {
    for (const auto& atlas : atlases) {
        if (atlas->ready) continue;
        const VkImage images[2] = {atlas->color.image, atlas->relief.image};
        std::array<VkImageMemoryBarrier2, 2> toClear{};
        for (int i = 0; i < 2; ++i) {
            toClear[i] = imageBarrier(images[i], 0, mipLevels_, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        barriers(cmd, toClear.data(), 2);
        // Black ground and flat relief under what no chunk covers - a hole.
        const VkClearColorValue clears[2] = {{{0.0f, 0.0f, 0.0f, 1.0f}}, {{0.5f, 0.5f, 1.0f, 1.0f}}};
        const VkImageSubresourceRange all{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                          .levelCount = mipLevels_, .baseArrayLayer = 0, .layerCount = 1};
        for (int i = 0; i < 2; ++i) {
            vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clears[i], 1, &all);
        }
        std::array<VkImageMemoryBarrier2, 2> toSample{};
        for (int i = 0; i < 2; ++i) {
            toSample[i] = imageBarrier(images[i], 0, mipLevels_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        barriers(cmd, toSample.data(), 2);
        atlas->ready = true;
    }
}

void TerrainBake::beginTile(VkCommandBuffer cmd, Atlas& atlas) {
    // Whatever the frames before drew from it is done before it is drawn into:
    // a barrier waits on everything earlier in the queue.
    std::array<VkImageMemoryBarrier2, 2> toDraw{};
    const VkImage images[2] = {atlas.color.image, atlas.relief.image};
    for (int i = 0; i < 2; ++i) {
        toDraw[i] = imageBarrier(images[i], 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    barriers(cmd, toDraw.data(), 2);

    // Loaded, not cleared: the tile's other chunks are already in it.
    std::array<VkRenderingAttachmentInfo, 2> targets{};
    const VkImageView views[2] = {atlas.colorTarget, atlas.reliefTarget};
    for (int i = 0; i < 2; ++i) {
        targets[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        targets[i].imageView = views[i];
        targets[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        targets[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        targets[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo info{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {.offset = {0, 0}, .extent = {kTerrainBakeAtlasSize, kTerrainBakeAtlasSize}};
    info.layerCount = 1;
    info.colorAttachmentCount = 2;
    info.pColorAttachments = targets.data();
    vkCmdBeginRendering(cmd, &info);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
}

void TerrainBake::cell(VkCommandBuffer cmd, int chunkX, int chunkY) {
    const float size = static_cast<float>(kTerrainBakeTexelsPerChunk);
    const VkViewport viewport{.x = static_cast<float>(chunkX) * size, .y = static_cast<float>(chunkY) * size,
                              .width = size, .height = size, .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{.offset = {static_cast<int32_t>(chunkX * kTerrainBakeTexelsPerChunk),
                                      static_cast<int32_t>(chunkY * kTerrainBakeTexelsPerChunk)},
                           .extent = {kTerrainBakeTexelsPerChunk, kTerrainBakeTexelsPerChunk}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void TerrainBake::endTile(VkCommandBuffer cmd, Atlas& atlas) {
    vkCmdEndRendering(cmd);

    // The mips, each the box filter of the one above, as the layers' own are.
    const VkImage images[2] = {atlas.color.image, atlas.relief.image};
    std::array<VkImageMemoryBarrier2, 4> start{};
    for (int i = 0; i < 2; ++i) {
        start[i * 2] = imageBarrier(images[i], 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT);
        start[i * 2 + 1] = imageBarrier(images[i], 1, mipLevels_ - 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    barriers(cmd, start.data(), 4);

    for (uint32_t level = 1; level < mipLevels_; ++level) {
        const int32_t from = static_cast<int32_t>(std::max(kTerrainBakeAtlasSize >> (level - 1), 1u));
        const int32_t to = static_cast<int32_t>(std::max(kTerrainBakeAtlasSize >> level, 1u));
        for (int i = 0; i < 2; ++i) {
            VkImageBlit blit{};
            blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level - 1,
                                   .baseArrayLayer = 0, .layerCount = 1};
            blit.srcOffsets[1] = {from, from, 1};
            blit.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level,
                                   .baseArrayLayer = 0, .layerCount = 1};
            blit.dstOffsets[1] = {to, to, 1};
            vkCmdBlitImage(cmd, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[i],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
        std::array<VkImageMemoryBarrier2, 2> next{};
        for (int i = 0; i < 2; ++i) {
            next[i] = imageBarrier(images[i], level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        barriers(cmd, next.data(), 2);
    }

    std::array<VkImageMemoryBarrier2, 2> toSample{};
    for (int i = 0; i < 2; ++i) {
        toSample[i] = imageBarrier(images[i], 0, mipLevels_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    barriers(cmd, toSample.data(), 2);
}

}  // namespace rendering
}  // namespace wowee
