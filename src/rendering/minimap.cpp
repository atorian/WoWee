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
#include "pipeline/minimap_tile_index.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>

namespace wowee {
namespace rendering {

// Push constant for tile composite vertex shader: where the image goes in the
// composite (see MinimapQuad).
struct MinimapTilePush {
    glm::vec2 corner;
    glm::vec2 down;
    glm::vec2 across;
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
    // A set per tile drawn per frame in flight, and the display's.
    std::vector<VkDescriptorSetLayout> layouts(MAX_DESC_SETS, samplerSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = MAX_DESC_SETS;
    allocInfo.pSetLayouts = layouts.data();

    // Checked, not assumed. On failure allSets holds nothing this code put
    // there, and every one of them is then written and bound - which is an
    // invalid handle in a live descriptor rather than a minimap that does
    // not draw.
    std::vector<VkDescriptorSet> allSets(MAX_DESC_SETS);
    if (vkAllocateDescriptorSets(device, &allocInfo, allSets.data()) != VK_SUCCESS) {
        LOG_ERROR("Minimap: failed to allocate descriptor sets");
        return false;
    }

    for (uint32_t f = 0; f < 2; f++)
        tileDescSets[f].assign(allSets.begin() + f * MAX_TILE_DRAWS, allSets.begin() + (f + 1) * MAX_TILE_DRAWS);
    displayDescSet = allSets[2 * MAX_TILE_DRAWS];

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

    // --- Tile pipeline layout: samplerSetLayout + the tile's place (vertex) ---
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
            // A building's tiles are cut-outs laid over one another; the
            // terrain's are opaque, so blending leaves them as they were.
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
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
        requested_.reset();
    }
}

namespace {

std::shared_ptr<VkTexture> uploadTile(VkContext* ctx, const pipeline::BLPImage& image, const std::string& path) {
    auto texture = std::shared_ptr<VkTexture>(new VkTexture, [ctx](VkTexture* tex) {
        ctx->deferAfterAllFrameFences([tex] { delete tex; });
    });
    if (!texture->upload(*ctx, image.data.data(), image.width, image.height, VK_FORMAT_R8G8B8A8_UNORM, false) ||
        !texture->createSampler(ctx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1)) {
        LOG_ERROR("Minimap tile ", path, " GPU upload failed; shown as no data");
        return nullptr;
    }
    return texture;
}

}  // namespace

// A bounded background request reads/decodes the lookup and tile files and
// uploads new images using its own submit lane. The producer only polls ready
// results; recording never reads archives or waits for uploads.
void Minimap::prepareTiles(const Request& request, const WmoMinimapPlacement* building, const glm::vec3& centre) {
    if (pendingTiles_.valid() && pendingTiles_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool current = pendingTiles_.current();
        auto tiles = pendingTiles_.get();
        if (tiles.lookup) trsLookup = tiles.lookup;
        if (tiles.areaFlagsRead) { areaFlags_ = tiles.areaFlags; areaFlagsRead_ = true; }
        if (current && tiles.request == request) {
            for (const auto& tile : tiles.tiles) {
                if (tile.file.empty()) continue;
                auto found = tileTextureCache.find(tile.file);
                if (found != tileTextureCache.end()) {
                    if (found->second.texture) tileLru_.splice(tileLru_.begin(), tileLru_, found->second.lru);
                } else if (tile.image) {
                    tileLru_.push_front(tile.file);
                    tileTextureCache.emplace(tile.file, CachedTile{tile.image, tileLru_.begin()});
                } else tileTextureCache.emplace(tile.file, CachedTile{nullptr, tileLru_.end()});
            }
            while (tileLru_.size() > MAX_TILE_CACHE) {
                tileTextureCache.erase(tileLru_.back());
                tileLru_.pop_back();
            }
            if (!tiles.missing.empty() && building && reportedMissing_.insert(building->model->tileStem).second) {
                LOG_INFO("Minimap: ", building->model->tileStem, " has no minimap image for ", tiles.missing.size(),
                         " of its tiles here (first ", tiles.missing.front(),
                         "); drawn without them, as the client does");
            }
            if (tiles.request.instance != shownInstance_) {
                shownInstance_ = tiles.request.instance;
                if (tiles.building() && building) {
                    LOG_INFO("Minimap: drawing ", building->model->tileStem, " from its own tiles (",
                             tiles.tiles.size(), " here, group ", tiles.request.group,
                             tiles.request.indoors ? ", indoors)" : ", outdoors)");
                } else {
                    LOG_INFO("Minimap: drawing the terrain of ", tiles.request.map);
                }
            }
            tiles.revision = ++tileRevision_;
            readyTiles_ = std::make_shared<Tiles>(std::move(tiles));
        }
    }
    if (pendingTiles_.valid() || !assetManager || requested_ == request) return;
    requested_ = request;
    auto* assets = assetManager;
    const auto* tables = gameTables_;
    auto* ctx = vkCtx;
    const auto lookup = trsLookup;
    const bool areaFlagsRead = areaFlagsRead_;
    std::optional<WmoMinimapPlacement> placement;
    if (building && request.instance != 0) placement = *building;
    std::unordered_map<std::string, std::shared_ptr<VkTexture>> existing;
    for (const auto& [hash, entry] : tileTextureCache) existing.emplace(hash, entry.texture);
    pendingTiles_ = tileJobs_.submit([assets, tables, ctx, request, centre, lookup, areaFlagsRead, placement,
                                      existing = std::move(existing)](const core::JobGroup::Token& token) {
        Tiles result;
        result.request = request;
        result.lookup = lookup;
        if (!token.current()) return result;
        if (!result.lookup) {
            auto data = assets->readFile("Textures\\Minimap\\md5translate.trs");
            if (data.empty()) LOG_WARNING("Minimap lookup md5translate.trs is unavailable; tiles show no data");
            result.lookup = std::make_shared<const pipeline::MinimapTileIndex>(
                pipeline::MinimapTileIndex::parse(data));
        }
        if (!areaFlagsRead) {
            result.areaFlagsRead = true;
            // Only WotLK's layout names the table's columns. Without it a
            // building's minimap outdoors - Dalaran's - shows the ground below.
            if (tables && tables->has("WMOAreaTable.dbc")) {
                result.areaFlags = std::make_shared<const WmoAreaFlags>(WmoAreaFlags::read(
                    pipeline::RequiredTable(*tables, "WMOAreaTable.dbc", "the minimap of a building seen from outside")));
            }
        }
        // Each image once: from the cache, or loaded and uploaded here.
        auto loaded = existing;
        const auto image = [&](const std::string& file, const std::string& name) -> std::shared_ptr<VkTexture> {
            if (auto cached = loaded.find(file); cached != loaded.end()) return cached->second;
            const auto path = "Textures\\Minimap\\" + file + ".blp";
            const auto blp = assets->loadTexture(path);
            if (!blp.isValid()) {
                LOG_WARNING("Minimap tile ", name, " (", path, ") failed to load; shown as no data");
                loaded.emplace(file, nullptr);
                return nullptr;
            }
            auto texture = uploadTile(ctx, blp, path);
            loaded.emplace(file, texture);
            return texture;
        };
        if (placement) {
            const auto& model = *placement->model;
            const float radius = kWmoMinimapRadius[request.zoom];
            result.area = wmoMinimapGatherBox(centre, radius);
            auto tiles = wmoMinimapTiles(model, request.group, request.indoors,
                                         wmoMinimapModelBox(result.area, placement->worldToModel));
            const glm::vec3 local = glm::vec3(placement->worldToModel * glm::vec4(centre, 1.0f));
            wmoMinimapDrawOrder(tiles, request.group, local.z);
            for (const auto& tile : tiles) {
                if (!token.current()) return result;
                const auto name = pipeline::MinimapTileIndex::buildingTileName(model.tileStem, tile.group, tile.x, tile.y);
                const auto* file = result.lookup->buildingTile(model.tileStem, tile.group, tile.x, tile.y);
                if (!file) { result.missing.push_back(name); continue; }
                auto texture = image(*file, name);
                if (!texture) continue;
                result.tiles.push_back({.image = texture, .file = *file,
                                        .quad = wmoMinimapQuad(tile, static_cast<int>(texture->getWidth()),
                                                               static_cast<int>(texture->getHeight()),
                                                               placement->modelToWorld, result.area)});
            }
            return result;
        }
        for (int dr = -1; dr <= 1; ++dr) for (int dc = -1; dc <= 1; ++dc) {
            if (!token.current()) return result;
            DrawnTile drawn;
            drawn.quad = {.corner = glm::vec2(dc + 1, dr + 1) / 3.0f, .down = {1.0f / 3.0f, 0.0f},
                          .across = {0.0f, 1.0f / 3.0f}};
            if (const auto* file = result.lookup->terrainTile(request.map, request.x + dr, request.y + dc)) {
                drawn.file = *file;
                drawn.image = image(*file, pipeline::MinimapTileIndex::terrainTileName(
                                               request.map, request.x + dr, request.y + dc));
            }
            result.tiles.push_back(std::move(drawn));
        }
        return result;
    });
}

void Minimap::updateTileDescriptors(uint32_t frameIdx, const Tiles& tiles) {
    const size_t count = std::min<size_t>(tiles.tiles.size(), MAX_TILE_DRAWS);
    std::vector<VkDescriptorImageInfo> imgInfos(count);
    std::vector<VkWriteDescriptorSet> writes(count);
    for (size_t i = 0; i < count; ++i) {
        VkTexture* tileTex = tiles.tiles[i].image.get();
        if (!tileTex || !tileTex->isValid()) tileTex = noDataTexture.get();
        imgInfos[i] = tileTex->descriptorInfo();
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = tileDescSets[frameIdx][i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgInfos[i];
    }
    if (count) vkUpdateDescriptorSets(vkCtx->getDevice(), static_cast<uint32_t>(count), writes.data(), 0, nullptr);
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

    const size_t count = std::min<size_t>(frame.tiles->tiles.size(), MAX_TILE_DRAWS);
    for (size_t i = 0; i < count; ++i) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tilePipelineLayout, 0, 1,
                                &tileDescSets[frameIdx][i], 0, nullptr);
        const auto& quad = frame.tiles->tiles[i].quad;
        const MinimapTilePush push{.corner = quad.corner, .down = quad.down, .across = quad.across};
        vkCmdPushConstants(cmd, tilePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 6, 1, 0, 0);
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
                                  float playerOrientation, bool hasPlayerOrientation,
                                  const WmoMinimapPlacement* building) {
    Frame frame;
    if (!enabled || !vkCtx || screenWidth <= 0 || screenHeight <= 0) return frame;
    auto [centreX, centreY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);
    Request request{.map = mapName, .x = centreX, .y = centreY};
    // The client's choice between the ground and a building's own tiles
    // (wow.exe 3.3.5a, 0x75d6c2): see wmoMinimapShown.
    if (building && building->model) {
        const auto& model = *building->model;
        const glm::vec3 local = glm::vec3(building->worldToModel * glm::vec4(centerWorldPos, 1.0f));
        const int group = model.groupAt(local);
        if (group >= 0) {
            const bool indoors = (model.groups[group].flags & kWmoGroupInterior) != 0;
            const uint32_t rootFlags = areaFlags_ ? areaFlags_->rootFlags(model.wmoId, building->nameSet) : 0;
            if (wmoMinimapShown(indoors, rootFlags)) {
                const float radius = kWmoMinimapRadius[indoorZoom_];
                request.instance = building->instance;
                request.group = group;
                request.indoors = indoors;
                request.zoom = indoorZoom_;
                request.cell = glm::ivec2(std::floor(centerWorldPos.x / radius), std::floor(centerWorldPos.y / radius));
            }
        }
    }
    prepareTiles(request, request.instance ? building : nullptr, centerWorldPos);
    frame.tiles = readyTiles_;
    frame.enabled = frame.tiles != nullptr;
    showingBuilding_ = frame.tiles && frame.tiles->building();

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
    float playerU = 0.0f, playerV = 0.0f, zoomRadius = 0.0f;
    if (showingBuilding_) {
        const auto& area = frame.tiles->area;
        const glm::vec2 uv = wmoMinimapAreaUV(centerWorldPos, area);
        playerU = uv.x;
        playerV = uv.y;
        zoomRadius = kWmoMinimapRadius[frame.tiles->request.zoom] / (area.max.x - area.min.x);
    } else {
        constexpr float TILE_SIZE = core::coords::TILE_SIZE;
        const int tileX = frame.tiles ? frame.tiles->request.x : centreX;
        const int tileY = frame.tiles ? frame.tiles->request.y : centreY;
        const float fracNS = 32.0f - static_cast<float>(tileX) - centerWorldPos.y / TILE_SIZE;
        const float fracEW = 32.0f - static_cast<float>(tileY) - centerWorldPos.x / TILE_SIZE;
        playerU = (1.0f + fracEW) / 3.0f;
        playerV = (1.0f + fracNS) / 3.0f;
        zoomRadius = viewRadius / (TILE_SIZE * 3.0f);
    }

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
