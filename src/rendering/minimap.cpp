#include "rendering/minimap.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <sstream>
#include <cmath>

namespace wowee {
namespace rendering {

// Push constant for tile composite vertex shader
struct MinimapTilePush {
    glm::vec2 gridOffset;  // 8 bytes
};

using MinimapDisplayPush = Minimap::Display;

Minimap::Minimap() = default;

Minimap::~Minimap() {
    shutdown();
}

/// The pipeline that draws the minimap's assembled texture to the screen.
///
/// initialize() builds it beside the tile pipeline that composes that texture;
/// recreatePipelines() rebuilds only this one, because the tile pipeline draws
/// into an offscreen pass a settings change does not touch. Both described it
/// identically, vertex layout included.
void Minimap::buildDisplayPipeline(VkDevice device,
                                   const VkPipelineShaderStageCreateInfo& vertStage,
                                   const VkPipelineShaderStageCreateInfo& fragStage) {
    // Two vec2s per vertex: position then texture coordinate.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };
    attrs[1] = { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float) };

    displayPipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({ binding }, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetPass_ != VK_NULL_HANDLE ? targetSamples_
                                                      : vkCtx->getMsaaSamples())
        .setLayout(displayPipelineLayout)
        .setRenderPass(targetPass_ != VK_NULL_HANDLE ? targetPass_
                                                     : vkCtx->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
}

bool Minimap::initialize(VkContext* ctx, VkDescriptorSetLayout /*perFrameLayout*/, int size) {
    vkCtx = ctx;
    mapSize = size;
    VkDevice device = vkCtx->getDevice();

    // --- Composite render target (768x768) ---
    compositeTarget = std::make_unique<VkRenderTarget>();
    if (!compositeTarget->create(*vkCtx, COMPOSITE_PX, COMPOSITE_PX)) {
        LOG_ERROR("Minimap: failed to create composite render target");
        return false;
    }

    // --- No-data fallback texture (dark blue-gray, 1x1) ---
    noDataTexture = std::make_unique<VkTexture>();
    uint8_t darkPixel[4] = { 12, 20, 30, 255 };
    noDataTexture->upload(*vkCtx, darkPixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    noDataTexture->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    // --- Shared quad vertex buffer (unit quad: pos2 + uv2) ---
    float quadVerts[] = {
        // pos (x,y), uv (u,v)
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f,
    };
    auto quadBuf = uploadBuffer(*vkCtx, quadVerts, sizeof(quadVerts),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    quadVB = quadBuf.buffer;
    quadVBAlloc = quadBuf.allocation;

    // --- Descriptor set layout: 1 combined image sampler at binding 0 (fragment) ---
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerSetLayout = createDescriptorSetLayout(device, { samplerBinding });

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_DESC_SETS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_DESC_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);

    // --- Allocate all descriptor sets ---
    // 18 tile sets (2 frames × 9 tiles) + 1 display set = 19 total
    std::vector<VkDescriptorSetLayout> layouts(19, samplerSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = 19;
    allocInfo.pSetLayouts = layouts.data();

    // Checked, not assumed. On failure allSets holds nothing this code put
    // there, and every one of the nineteen is then written and bound - which
    // is an invalid handle in a live descriptor rather than a minimap that
    // does not draw.
    VkDescriptorSet allSets[19];
    if (vkAllocateDescriptorSets(device, &allocInfo, allSets) != VK_SUCCESS) {
        LOG_ERROR("Minimap: failed to allocate descriptor sets");
        return false;
    }

    for (int f = 0; f < 2; f++)
        for (int t = 0; t < 9; t++)
            tileDescSets[f][t] = allSets[f * 9 + t];
    displayDescSet = allSets[18];

    // --- Write display descriptor set → composite render target ---
    VkDescriptorImageInfo compositeImgInfo = compositeTarget->descriptorInfo();
    VkWriteDescriptorSet displayWrite{};
    displayWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    displayWrite.dstSet = displayDescSet;
    displayWrite.dstBinding = 0;
    displayWrite.descriptorCount = 1;
    displayWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    displayWrite.pImageInfo = &compositeImgInfo;
    vkUpdateDescriptorSets(device, 1, &displayWrite, 0, nullptr);

    // --- Tile pipeline layout: samplerSetLayout + 8-byte push constant (vertex) ---
    VkPushConstantRange tilePush{};
    tilePush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    tilePush.offset = 0;
    tilePush.size = sizeof(MinimapTilePush);
    tilePipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { tilePush });

    // --- Display pipeline layout: samplerSetLayout + 40-byte push constant (vert+frag) ---
    VkPushConstantRange displayPush{};
    displayPush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    displayPush.offset = 0;
    displayPush.size = sizeof(MinimapDisplayPush);
    displayPipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { displayPush });

    // --- Vertex input: pos2 (loc 0) + uv2 (loc 1), stride 16 ---
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };                    // aPos
    attrs[1] = { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float) };    // aUV

    // --- Load tile shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_tile.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_tile.frag.spv")) {
            LOG_ERROR("Minimap: failed to load tile shaders");
            return false;
        }

        tilePipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendDisabled())
            .setLayout(tilePipelineLayout)
            .setRenderPass(compositeTarget->getRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx->getPipelineCache());

        vs.destroy();
        fs.destroy();
    }

    // --- Load display shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
            LOG_ERROR("Minimap: failed to load display shaders");
            return false;
        }

        buildDisplayPipeline(device, vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                             fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT));

        vs.destroy();
        fs.destroy();
    }

    if (!tilePipeline || !displayPipeline) {
        LOG_ERROR("Minimap: failed to create pipelines");
        return false;
    }

    LOG_INFO("Minimap initialized (", mapSize, "x", mapSize, " screen, ",
             COMPOSITE_PX, "x", COMPOSITE_PX, " composite)");
    return true;
}

void Minimap::shutdown() {
    tileJobs_.cancel();
    tileJobs_.wait();
    pendingTiles_ = {};
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();

    vkCtx->waitIdle();

    destroy(device, tilePipeline);
    destroy(device, displayPipeline);
    destroy(device, tilePipelineLayout);
    destroy(device, displayPipelineLayout);
    destroy(device, descPool);
    destroy(device, samplerSetLayout);

    destroy(alloc, quadVB, quadVBAlloc);

    readyTiles_.reset();
    tileTextureCache.clear();
    tileLru_.clear();

    if (noDataTexture) { noDataTexture->destroy(device, alloc); noDataTexture.reset(); }
    if (compositeTarget) { compositeTarget->destroy(device, alloc); compositeTarget.reset(); }

    vkCtx = nullptr;
}

void Minimap::recreatePipelines() {
    if (!vkCtx || !displayPipelineLayout) return;
    VkDevice device = vkCtx->getDevice();

    DeferredResources retired(*vkCtx);
    retired.destroy(displayPipeline);

    VkShaderModule vs, fs;
    if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
        !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
        LOG_ERROR("Minimap: failed to reload display shaders for pipeline recreation");
        return;
    }

    buildDisplayPipeline(device, vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                         fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT));

    vs.destroy();
    fs.destroy();

    LOG_INFO("Minimap: display pipeline recreated with MSAA ", static_cast<int>(vkCtx->getMsaaSamples()), "x");
}

void Minimap::setMapName(const std::string& name) {
    if (mapName != name) {
        mapName = name;
        requestedMap_.clear();
    }
}

// A bounded background request reads/decodes the lookup and tile files and
// uploads new images using its own submit lane. The producer only polls ready
// results; recording never reads archives or waits for uploads.
void Minimap::prepareTiles(int x, int y) {
    if (pendingTiles_.valid() && pendingTiles_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool current = pendingTiles_.current();
        auto tiles = pendingTiles_.get();
        if (current && tiles.map == mapName && tiles.x == x && tiles.y == y) {
            trsLookup = tiles.lookup;
            for (size_t i = 0; i < tiles.images.size(); ++i) {
                const auto& image = tiles.images[i];
                const auto& key = tiles.hashes[i];
                if (key.empty()) continue;
                auto found = tileTextureCache.find(key);
                if (found != tileTextureCache.end()) {
                    if (found->second.texture) tileLru_.splice(tileLru_.begin(), tileLru_, found->second.lru);
                } else if (image) {
                    tileLru_.push_front(key); tileTextureCache.emplace(key, CachedTile{image, tileLru_.begin()});
                } else tileTextureCache.emplace(key, CachedTile{nullptr, tileLru_.end()});
            }
            while (tileLru_.size() > MAX_TILE_CACHE) {
                tileTextureCache.erase(tileLru_.back());
                tileLru_.pop_back();
            }
            tiles.revision = ++tileRevision_;
            readyTiles_ = std::make_shared<Tiles>(std::move(tiles));
        }
    }
    if (pendingTiles_.valid() || !assetManager ||
        (requestedMap_ == mapName && requestedX_ == x && requestedY_ == y)) return;
    requestedMap_ = mapName; requestedX_ = x; requestedY_ = y;
    auto* assets = assetManager;
    auto* ctx = vkCtx;
    const auto map = mapName;
    const auto lookup = trsLookup;
    std::unordered_map<std::string, std::shared_ptr<VkTexture>> existing;
    for (const auto& [hash, entry] : tileTextureCache) existing.emplace(hash, entry.texture);
    pendingTiles_ = tileJobs_.submit([assets, ctx, map, x, y, lookup, existing = std::move(existing)](const core::JobGroup::Token& token) {
        Tiles result;
        result.map = map; result.x = x; result.y = y; result.lookup = lookup;
        if (!token.current()) return result;
        if (!result.lookup) {
            auto parsed = std::make_shared<std::unordered_map<std::string, std::string>>();
            auto data = assets->readFile("Textures\\Minimap\\md5translate.trs");
            if (data.empty()) LOG_WARNING("Minimap lookup md5translate.trs is unavailable; tiles show no data");
            std::istringstream stream(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                auto key = line.substr(0, tab), hash = line.substr(tab + 1);
                if (key.ends_with(".blp")) key.resize(key.size() - 4);
                if (hash.ends_with(".blp")) hash.resize(hash.size() - 4);
                (*parsed)[std::move(key)] = std::move(hash);
            }
            result.lookup = std::move(parsed);
        }
        int slot = 0;
        for (int dr = -1; dr <= 1; ++dr) for (int dc = -1; dc <= 1; ++dc, ++slot) {
            if (!token.current()) return result;
            const auto key = map + "\\map" + std::to_string(x + dr) + "_" + std::to_string(y + dc);
            const auto found = result.lookup->find(key);
            if (found == result.lookup->end()) continue;
            result.hashes[slot] = found->second;
            if (auto cached = existing.find(found->second); cached != existing.end()) {
                result.images[slot] = cached->second;
                continue;
            }
            const auto path = "Textures\\Minimap\\" + found->second + ".blp";
            const auto image = assets->loadTexture(path);
            if (!image.isValid()) {
                LOG_WARNING("Minimap tile ", key, " (", path, ") failed to load; shown as no data");
                continue;
            }
            auto texture = std::shared_ptr<VkTexture>(new VkTexture, [ctx](VkTexture* tex) {
                ctx->deferAfterAllFrameFences([tex] { delete tex; });
            });
            if (!texture->upload(*ctx, image.data.data(), image.width, image.height,
                                 VK_FORMAT_R8G8B8A8_UNORM, false) ||
                !texture->createSampler(ctx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1)) {
                LOG_ERROR("Minimap tile ", path, " GPU upload failed; shown as no data");
                continue;
            }
            result.images[slot] = std::move(texture);
        }
        return result;
    });
}

void Minimap::updateTileDescriptors(uint32_t frameIdx, const Tiles& tiles) {
    constexpr int kTileCount = 9; // 3x3 grid
    VkDevice device = vkCtx->getDevice();
    std::array<VkDescriptorImageInfo, kTileCount> imgInfos{};
    std::array<VkWriteDescriptorSet, kTileCount> writes{};
    int slot = 0;

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            VkTexture* tileTex = tiles.images[slot].get();
            if (!tileTex || !tileTex->isValid()) tileTex = noDataTexture.get();

            imgInfos[slot] = tileTex->descriptorInfo();

            writes[slot] = {};
            writes[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[slot].dstSet = tileDescSets[frameIdx][slot];
            writes[slot].dstBinding = 0;
            writes[slot].descriptorCount = 1;
            writes[slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[slot].pImageInfo = &imgInfos[slot];
            slot++;
        }
    }

    vkUpdateDescriptorSets(device, kTileCount, writes.data(), 0, nullptr);
}

// --------------------------------------------------------
// Off-screen composite pass (call BEFORE main render pass)
// --------------------------------------------------------

void Minimap::recordComposite(VkCommandBuffer cmd, const Frame& frame) {
    if (!frame.enabled || !frame.tiles || !compositeTarget ||
        frame.tiles->revision == compositeRevision_) return;
    const uint32_t frameIdx = vkCtx->getCurrentFrame();
    updateTileDescriptors(frameIdx, *frame.tiles);

    // Begin off-screen render pass
    VkClearColorValue clearColor = {{ 0.05f, 0.08f, 0.12f, 1.0f }};
    compositeTarget->beginPass(cmd, clearColor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tilePipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // Draw 3x3 tile grid
    int slot = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    tilePipelineLayout, 0, 1,
                                    &tileDescSets[frameIdx][slot], 0, nullptr);

            MinimapTilePush push{};
            push.gridOffset = glm::vec2(static_cast<float>(dc + 1),
                                        static_cast<float>(dr + 1));
            vkCmdPushConstants(cmd, tilePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);

            vkCmdDraw(cmd, 6, 1, 0, 0);
            slot++;
        }
    }

    compositeTarget->endPass(cmd);

    compositeRevision_ = frame.tiles->revision;
    hasCachedFrame = true;
}

// --------------------------------------------------------
// Display quad (call INSIDE main render pass)
// --------------------------------------------------------

Minimap::Frame Minimap::extract(const Camera& playerCamera, const glm::vec3& centerWorldPos,
                                  int screenWidth, int screenHeight,
                                  float playerOrientation, bool hasPlayerOrientation) {
    Frame frame;
    if (!enabled || !vkCtx || screenWidth <= 0 || screenHeight <= 0) return frame;
    auto [centreX, centreY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);
    prepareTiles(centreX, centreY);
    frame.tiles = readyTiles_;
    frame.enabled = frame.tiles != nullptr;

    // Top-right corner, unless something asked for a particular rect - which
    // is what happens when FrameXML owns the minimap and the map has to sit
    // inside the frame it drew.
    float margin = 10.0f;
    float pixelW, pixelH, x, y;
    if (haveRect_ && rectW_ > 0.0f && rectH_ > 0.0f) {
        pixelW = rectW_ / screenWidth;
        pixelH = rectH_ / screenHeight;
        x = rectX_ / screenWidth;
        y = rectY_ / screenHeight;   // y=0 is the top edge in Vulkan
    } else {
        pixelW = static_cast<float>(mapSize) / screenWidth;
        pixelH = static_cast<float>(mapSize) / screenHeight;
        x = 1.0f - pixelW - margin / screenWidth;
        y = margin / screenHeight;
    }

    // Compute player's UV in the composite texture
    constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    const int tileX = frame.tiles ? frame.tiles->x : centreX;
    const int tileY = frame.tiles ? frame.tiles->y : centreY;

    float fracNS = 32.0f - static_cast<float>(tileX) - centerWorldPos.y / TILE_SIZE;
    float fracEW = 32.0f - static_cast<float>(tileY) - centerWorldPos.x / TILE_SIZE;

    float playerU = (1.0f + fracEW) / 3.0f;
    float playerV = (1.0f + fracNS) / 3.0f;

    float zoomRadius = viewRadius / (TILE_SIZE * 3.0f);

    // Rotating with the camera is off everywhere: the saved setting is read and
    // dropped in loadSettings, since "Stabilize transports and correct minimap
    // orientation". This is why, as far as it can be worked out without the
    // client on screen.
    //
    // The two modes disagree by half a turn. Take the heading where
    // atan2(-fwd.x, fwd.y) is 0. North-up draws the arrow at pi - that same
    // expression, which is pi: pointing down, so that heading renders as south.
    // Rotating mode turns the map by the expression itself, which is 0 - no
    // rotation at all - while pinning the arrow up. So the map says the player
    // faces north and the arrow agrees, and both are half a turn from where
    // north-up puts them.
    //
    // North-up is the mode that has been looked at, so its arrow is the one to
    // trust: the map wants turning by the negative of where that arrow points,
    // not by the expression the arrow is built from.
    //
    // The direction is a second question and not settled here. What the shader
    // does with this angle is a mirror as well as a turn - the matrix it builds
    // has determinant -1 - so the map may also rotate the wrong way once the
    // half turn is accounted for. That mirror is right for north-up, where the
    // angle is zero, which is what makes this hard to reason about and easy to
    // check by turning it on and walking north.
    float rotation = 0.0f;
    if (rotateWithCamera) {
        glm::vec3 fwd = playerCamera.getForward();
        rotation = std::atan2(-fwd.x, fwd.y);
    }

    float arrowRotation = 0.0f;
    if (!rotateWithCamera) {
        if (hasPlayerOrientation) {
            arrowRotation = playerOrientation;
        } else {
            glm::vec3 fwd = playerCamera.getForward();
            arrowRotation = glm::pi<float>() - std::atan2(-fwd.x, fwd.y);
        }
    } else if (hasPlayerOrientation) {
        // Show character facing relative to the rotated map
        arrowRotation = playerOrientation + rotation;
    }

    MinimapDisplayPush push{};
    push.rect = glm::vec4(x, y, pixelW, pixelH);
    push.playerUV = glm::vec2(playerU, playerV);
    push.rotation = rotation;
    push.arrowRotation = arrowRotation;
    push.zoomRadius = zoomRadius;
    push.squareShape = squareShape ? 1 : 0;
    push.opacity = opacity_;

    frame.display = push;
    return frame;
}

void Minimap::record(VkCommandBuffer cmd, const Frame& frame) {
    if (!frame.enabled || !hasCachedFrame || !displayPipeline) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            displayPipelineLayout, 0, 1, &displayDescSet, 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);
    vkCmdPushConstants(cmd, displayPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(frame.display), &frame.display);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
