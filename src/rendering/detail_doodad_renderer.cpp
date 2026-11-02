#include "rendering/detail_doodad_renderer.hpp"

#include <algorithm>
#include <limits>

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "pipeline/adt_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/texture_upload.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

namespace {

/// Chunks given their clutter per frame. A chunk's layout and mesh take well
/// under a millisecond; a few per frame fills a view of them in a second or
/// so without a frame paying for all of it at once.
constexpr size_t kBuildsPerFrame = 4;

/// Texture sets the pool holds: one per distinct texture the clutter of a
/// session uses, which is a few per zone.
constexpr uint32_t kMaxTextures = 1024;

/// Clutter stands above the ground it grows from; a chunk's bounds for the
/// range and view tests reach this far over its highest vertex.
constexpr float kClutterHeight = 3.0f;

uint64_t chunkKey(uint32_t x, uint32_t y) {
    return (static_cast<uint64_t>(x) << 32) | y;
}

/// The chunk's terrain box, heightened by what may stand on it.
void chunkBounds(const pipeline::MapChunk& chunk, glm::vec3& lo, glm::vec3& hi) {
    constexpr float size = core::coords::TILE_SIZE / 16.0f;
    float minH = std::numeric_limits<float>::max();
    float maxH = std::numeric_limits<float>::lowest();
    for (float h : chunk.heightMap.heights) {
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
    }
    lo = glm::vec3(chunk.position[0] - size, chunk.position[1] - size, chunk.position[2] + minH);
    hi = glm::vec3(chunk.position[0], chunk.position[1], chunk.position[2] + maxH + kClutterHeight);
}

/// How far ahead of the camera the box's nearest corner is - the distance
/// the client measures a chunk's clutter by (0x790650, 0x7c3ea3).
float nearestDepth(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& eye,
                   const glm::vec3& forward) {
    const glm::vec3 nearest(forward.x >= 0.0f ? lo.x : hi.x, forward.y >= 0.0f ? lo.y : hi.y,
                            forward.z >= 0.0f ? lo.z : hi.z);
    return glm::dot(nearest - eye, forward);
}

} // namespace

DetailDoodadRenderer::ChunkGpu::~ChunkGpu() {
    if (!ctx) return;
    // A frame in flight may still draw from them.
    VmaAllocator allocator = ctx->getAllocator();
    ctx->deferAfterAllFrameFences([allocator, v = vertices, i = indices]() mutable {
        destroyBuffer(allocator, v);
        destroyBuffer(allocator, i);
    });
}

DetailDoodadRenderer::~DetailDoodadRenderer() { shutdown(); }

bool DetailDoodadRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    VkDevice device = ctx->getDevice();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureLayout_ = createDescriptorSetLayout(device, {binding});
    if (textureLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("Ground clutter: could not create its texture set layout; no clutter is drawn");
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets = kMaxTextures;
    poolCi.poolSizeCount = 1;
    poolCi.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolCi, nullptr, &texturePool_) != VK_SUCCESS) {
        LOG_ERROR("Ground clutter: could not create its texture pool; no clutter is drawn");
        return false;
    }

    if (!buildPipeline()) {
        LOG_ERROR("Ground clutter: could not build its pipeline; no clutter is drawn");
        return false;
    }
    return true;
}

bool DetailDoodadRenderer::buildPipeline() {
    VkDevice device = vkCtx_->getDevice();
    destroyPipeline(device, pipeline_, pipelineLayout_);

    auto shaders = loadShaderPair(device, "assets/shaders/detail_doodad.vert.spv",
                                  "assets/shaders/detail_doodad.frag.spv", "ground clutter");
    if (!shaders) return false;

    VkPushConstantRange fadeRange{};
    fadeRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    fadeRange.offset = 0;
    fadeRange.size = sizeof(glm::vec4);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, textureLayout_}, {fadeRange});
    if (pipelineLayout_ == VK_NULL_HANDLE) return false;

    using Vertex = pipeline::DetailChunkMesh::Vertex;
    static_assert(sizeof(Vertex) == 32, "detail_doodad.vert.glsl reads 32-byte vertices");
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::vector<VkVertexInputAttributeDescription> attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, normal))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, uv))},
    };

    pipeline_ = PipelineBuilder()
                    .setShaders(shaders.vertStage, shaders.fragStage)
                    .setVertexInput({binding}, attributes)
                    .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                    // Cards seen from both sides, as the client draws them.
                    .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                    .setDepthTest(true, true, VK_COMPARE_OP_LESS)
                    .setColorBlendAttachment(PipelineBuilder::blendDisabled())
                    .setMultisample(vkCtx_->getMsaaSamples())
                    .setLayout(pipelineLayout_)
                    .setRenderPass(vkCtx_->getImGuiRenderPass())
                    .setDynamicStates(viewportAndScissorDynamic())
                    .build(device, vkCtx_->getPipelineCache());
    return pipeline_ != VK_NULL_HANDLE;
}

void DetailDoodadRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    if (!buildPipeline()) LOG_ERROR("Ground clutter: pipeline rebuild failed; no clutter is drawn");
}

void DetailDoodadRenderer::shutdown() {
    if (!vkCtx_) return;
    chunks_.clear();
    textures_.clear();
    VkDevice device = vkCtx_->getDevice();
    destroyPipeline(device, pipeline_, pipelineLayout_);
    // The sets go with the pool.
    destroy(device, texturePool_);
    destroy(device, textureLayout_);
    vkCtx_ = nullptr;
}

void DetailDoodadRenderer::addModel(uint32_t doodadId, pipeline::DetailDoodadModel model,
                                    std::vector<std::pair<std::string, pipeline::BLPImage>> images) {
    if (!vkCtx_ || models_.count(doodadId)) return;
    for (auto& [path, image] : images) {
        if (textures_.count(path)) continue;
        Texture texture;
        texture.image = cacheStoredTexture(*vkCtx_, path, image);
        if (!texture.image) {
            // cacheStoredTexture has said why.
            LOG_WARNING("Ground clutter: texture ", path, " of doodad ", doodadId,
                        " did not upload; the clutter that uses it is not drawn");
            textures_.emplace(path, std::move(texture));
            continue;
        }
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = texturePool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &textureLayout_;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &texture.set) != VK_SUCCESS) {
            LOG_ERROR("Ground clutter: out of texture sets at ", textures_.size(), " textures; ",
                      path, " and the clutter that uses it is not drawn");
            texture.set = VK_NULL_HANDLE;
        } else {
            const VkDescriptorImageInfo info = texture.image->descriptorInfo();
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = texture.set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
        }
        textures_.emplace(path, std::move(texture));
    }
    for (const auto& batch : model.batches) {
        if (!textures_.count(batch.texture) && reportedTextures_.insert(batch.texture).second) {
            LOG_WARNING("Ground clutter: doodad ", doodadId, " names texture ", batch.texture,
                        " that was not read; that part of it is not drawn");
        }
    }
    models_.emplace(doodadId, std::move(model));
}

void DetailDoodadRenderer::modelFailed(uint32_t doodadId) {
    if (models_.count(doodadId)) return;
    failedModels_.insert(doodadId);
}

bool DetailDoodadRenderer::hasModel(uint32_t doodadId) const {
    return models_.count(doodadId) != 0 || failedModels_.count(doodadId) != 0;
}

void DetailDoodadRenderer::setDensity(uint32_t quadDraws) {
    quadDraws = std::clamp(quadDraws, pipeline::kMinGroundEffectDensity, pipeline::kMaxGroundEffectDensity);
    if (quadDraws == density_) return;
    density_ = quadDraws;
    // Every chunk is laid out again; update() rebuilds them as they come up.
    chunks_.clear();
}

void DetailDoodadRenderer::setDistance(float yards) {
    distance_ = std::clamp(yards, 1.0f, pipeline::kMaxGroundEffectDist);
}

VkDescriptorSet DetailDoodadRenderer::textureSet(const std::string& path) const {
    const auto it = textures_.find(path);
    return it == textures_.end() ? VK_NULL_HANDLE : it->second.set;
}

bool DetailDoodadRenderer::buildChunk(const NearbyChunk& source,
                                      const pipeline::GroundEffectTables& tables, Entry& out) {
    const pipeline::DetailDoodadLayout layout =
        pipeline::placeDetailDoodads(*source.chunk, source.globalX, source.globalY, tables, density_);

    for (uint32_t id : layout.unknownEffects) {
        if (reportedEffects_.insert(id).second) {
            LOG_WARNING("Ground clutter: a texture layer names ground effect ", id,
                        ", which GroundEffectTexture.dbc does not have; its quads grow nothing");
        }
    }
    for (uint32_t id : layout.unknownDoodads) {
        if (reportedDoodads_.insert(id).second) {
            LOG_WARNING("Ground clutter: a ground effect names doodad ", id,
                        ", which GroundEffectDoodad.dbc does not have; it is not planted");
        }
    }
    if (layout.missingLayers > 0 && !reportedMissingLayers_) {
        reportedMissingLayers_ = true;
        LOG_WARNING("Ground clutter: chunk (", source.globalX, ",", source.globalY, ") has ",
                    layout.missingLayers, " quads whose low-quality texture map names a layer the "
                    "chunk does not have; they grow nothing (said once)");
    }

    for (const auto& p : layout.placements) {
        if (!hasModel(p.doodadId)) return false;  // still on its way
    }

    const pipeline::DetailChunkMesh mesh = pipeline::buildDetailChunkMesh(
        layout.placements, tables, [this](uint32_t id) -> const pipeline::DetailDoodadModel* {
            const auto it = models_.find(id);
            return it == models_.end() ? nullptr : &it->second;
        });
    out.gpu.reset();
    if (mesh.indices.empty()) return true;

    auto gpu = std::make_shared<ChunkGpu>();
    gpu->boundsMin = mesh.boundsMin;
    gpu->boundsMax = mesh.boundsMax;
    for (const auto& b : mesh.batches) {
        const VkDescriptorSet set = textureSet(b.texture);
        if (set == VK_NULL_HANDLE) continue;  // reported when its model came
        gpu->batches.push_back({set, b.firstIndex, b.indexCount});
    }
    if (gpu->batches.empty()) return true;

    gpu->uploaded = std::make_shared<std::atomic<int>>(0);
    vkCtx_->beginUploadBatch();
    gpu->vertices = uploadBuffer(*vkCtx_, mesh.vertices.data(),
                                 mesh.vertices.size() * sizeof(mesh.vertices[0]),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpu->indices = uploadBuffer(*vkCtx_, mesh.indices.data(),
                                mesh.indices.size() * sizeof(mesh.indices[0]),
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    vkCtx_->endUploadBatch(gpu->uploaded);
    gpu->ctx = vkCtx_;
    if (gpu->vertices.buffer == VK_NULL_HANDLE || gpu->indices.buffer == VK_NULL_HANDLE) {
        LOG_ERROR("Ground clutter: could not upload chunk (", source.globalX, ",", source.globalY,
                  "), ", mesh.vertices.size(), " vertices; it is drawn without clutter");
        return true;  // gpu dropped: the chunk stays bare rather than retrying every frame
    }
    out.gpu = std::move(gpu);
    return true;
}

void DetailDoodadRenderer::update(const Camera& camera, const std::vector<NearbyChunk>& nearby,
                                  const pipeline::GroundEffectTables& tables) {
    if (!isReady()) return;

    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());
    const glm::vec3 eye = camera.getPosition();
    const glm::vec3 forward = glm::normalize(camera.getForward());

    // Let go of the chunks no longer offered.
    std::unordered_set<uint64_t> offered;
    offered.reserve(nearby.size());
    for (const NearbyChunk& n : nearby) offered.insert(chunkKey(n.globalX, n.globalY));
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (!offered.count(it->first)) {
            it = chunks_.erase(it);
            continue;
        }
        auto& gpu = it->second.gpu;
        if (gpu && gpu->uploaded->load(std::memory_order_acquire) < 0) {
            LOG_ERROR("Ground clutter: the upload of chunk (", it->first >> 32, ",",
                      it->first & 0xffffffffu, ") failed; it is drawn without clutter");
            gpu.reset();
        }
        ++it;
    }

    // The client lays a chunk out when it draws it: in view, and nearer than
    // groundEffectDist ahead.
    struct Want { float depth; const NearbyChunk* source; };
    std::vector<Want> wanted;
    for (const NearbyChunk& n : nearby) {
        if (!n.chunk || chunks_.count(chunkKey(n.globalX, n.globalY))) continue;
        glm::vec3 lo, hi;
        chunkBounds(*n.chunk, lo, hi);
        const float depth = nearestDepth(lo, hi, eye, forward);
        if (depth >= distance_ || !frustum.intersectsAABB(lo, hi)) continue;
        wanted.push_back({depth, &n});
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const Want& a, const Want& b) { return a.depth < b.depth; });

    size_t built = 0;
    for (const Want& w : wanted) {
        if (built >= kBuildsPerFrame) break;
        Entry entry;
        if (!buildChunk(*w.source, tables, entry)) continue;
        chunks_.emplace(chunkKey(w.source->globalX, w.source->globalY), std::move(entry));
        ++built;
    }
    waiting_ = wanted.size() - built;
}

size_t DetailDoodadRenderer::pendingChunks() const {
    size_t uploading = 0;
    for (const auto& [key, entry] : chunks_) {
        if (entry.gpu && entry.gpu->uploaded->load(std::memory_order_acquire) == 0) ++uploading;
    }
    return waiting_ + uploading;
}

DetailDoodadRenderer::Frame DetailDoodadRenderer::extract(const Camera& camera) const {
    Frame frame;
    frame.distance = distance_;
    if (!isReady()) return frame;
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());
    const glm::vec3 eye = camera.getPosition();
    const glm::vec3 forward = glm::normalize(camera.getForward());
    for (const auto& [key, entry] : chunks_) {
        const auto& gpu = entry.gpu;
        if (!gpu || gpu->uploaded->load(std::memory_order_acquire) != 1) continue;
        if (nearestDepth(gpu->boundsMin, gpu->boundsMax, eye, forward) >= distance_) continue;
        if (!frustum.intersectsAABB(gpu->boundsMin, gpu->boundsMax)) continue;
        frame.chunks.push_back(gpu);
    }
    return frame;
}

void DetailDoodadRenderer::record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                  const Frame& frame) const {
    if (pipeline_ == VK_NULL_HANDLE || frame.chunks.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &perFrameSet, 0, nullptr);
    const glm::vec4 fade(frame.distance, frame.distance * pipeline::kGroundEffectFadeStart, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(fade), &fade);
    for (const auto& chunk : frame.chunks) {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &chunk->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, chunk->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const auto& batch : chunk->batches) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                    &batch.texture, 0, nullptr);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.firstIndex, 0, 0);
        }
    }
}

} // namespace rendering
} // namespace wowee
