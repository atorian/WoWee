#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "pipeline/blp_loader.hpp"
#include "pipeline/detail_doodads.hpp"
#include "rendering/texture_cache.hpp"
#include "rendering/vk_context.hpp"

namespace wowee {
namespace rendering {

class Camera;

/// Ground clutter - the grass, flowers, stones and bones on the terrain -
/// drawn the way the 3.3.5a client draws it (WoW.exe 12340: CMapChunk::
/// CreateDetailDoodads 0x7d3390, CDetailDoodadInst, the draw at 0x7b15d0).
///
/// A chunk gets its clutter when it is in view and its nearest point is less
/// than groundEffectDist ahead of the camera: laid out by
/// pipeline::placeDetailDoodads, every doodad's mesh moved onto its spot and
/// put in one vertex and index buffer for the chunk, a batch per texture. The
/// meshes are drawn as they stand in their files - the client neither animates
/// clutter nor moves it with the wind - lit by the ground's normal, alpha-
/// tested, and faded out over the last 15% of the distance.
///
/// The doodads' models and textures come from the tile loader, which reads
/// them on its workers (addModel / modelFailed); a chunk whose clutter needs a
/// model not yet delivered waits for it.
class DetailDoodadRenderer {
public:
    DetailDoodadRenderer() = default;
    ~DetailDoodadRenderer();

    DetailDoodadRenderer(const DetailDoodadRenderer&) = delete;
    DetailDoodadRenderer& operator=(const DetailDoodadRenderer&) = delete;

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    /// Rebuild the pipeline against the current render pass (MSAA, swapchain).
    void recreatePipelines();
    [[nodiscard]] bool isReady() const { return pipeline_ != VK_NULL_HANDLE; }

    /// A doodad's model, and the decoded images of the textures it names.
    void addModel(uint32_t doodadId, pipeline::DetailDoodadModel model,
                  std::vector<std::pair<std::string, pipeline::BLPImage>> images);
    /// A doodad whose model could not be read: its clutter is left out of the
    /// chunks that plant it rather than waited for.
    void modelFailed(uint32_t doodadId);
    /// Whether the doodad's model was delivered or failed.
    [[nodiscard]] bool hasModel(uint32_t doodadId) const;

    /// groundEffectDensity: how many quads of each chunk are planted. Every
    /// chunk is laid out again.
    void setDensity(uint32_t quadDraws);
    [[nodiscard]] uint32_t density() const { return density_; }
    /// groundEffectDist, yards.
    void setDistance(float yards);
    [[nodiscard]] float distance() const { return distance_; }

    /// A terrain chunk near the camera, with its column and row in the map.
    struct NearbyChunk {
        const pipeline::MapChunk* chunk = nullptr;
        uint32_t globalX = 0;
        uint32_t globalY = 0;
    };
    /// Per frame, on the main thread: give the chunks in view and in range
    /// their clutter, a few per frame, nearest first; let go of the clutter of
    /// every chunk not in `nearby`.
    void update(const Camera& camera, const std::vector<NearbyChunk>& nearby,
                const pipeline::GroundEffectTables& tables);
    /// Chunks in view and in range whose clutter is not drawn yet: not laid
    /// out, waiting for a model, or still uploading.
    [[nodiscard]] size_t pendingChunks() const;

    /// One chunk's clutter on the device. Immutable once uploaded; freed once
    /// no frame that may draw it is in flight.
    struct ChunkGpu {
        struct Batch {
            VkDescriptorSet texture = VK_NULL_HANDLE;
            uint32_t firstIndex = 0;
            uint32_t indexCount = 0;
        };
        VkContext* ctx = nullptr;
        AllocatedBuffer vertices{};
        AllocatedBuffer indices{};
        std::vector<Batch> batches;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
        VkContext::UploadCompletion uploaded;
        ~ChunkGpu();
    };

    /// What one frame draws: the chunks in view and in range.
    struct Frame {
        std::vector<std::shared_ptr<const ChunkGpu>> chunks;
        float distance = pipeline::kDefaultGroundEffectDist;
    };
    [[nodiscard]] Frame extract(const Camera& camera) const;
    /// Inside the render pass, after the terrain.
    void record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Frame& frame) const;

private:
    struct Texture {
        TextureRef image;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct Entry {
        /// Null for a chunk that grows nothing.
        std::shared_ptr<ChunkGpu> gpu;
    };

    bool buildPipeline();
    /// False when the chunk waits for a model.
    bool buildChunk(const NearbyChunk& source, const pipeline::GroundEffectTables& tables, Entry& out);
    VkDescriptorSet textureSet(const std::string& path) const;

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool texturePool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    uint32_t density_ = pipeline::kDefaultGroundEffectDensity;
    float distance_ = pipeline::kDefaultGroundEffectDist;

    std::unordered_map<uint32_t, pipeline::DetailDoodadModel> models_;
    std::unordered_set<uint32_t> failedModels_;
    std::unordered_map<std::string, Texture> textures_;
    std::unordered_map<uint64_t, Entry> chunks_;  // by globalX << 32 | globalY
    /// Chunks the last update wanted and did not lay out.
    size_t waiting_ = 0;

    // Said once each.
    std::unordered_set<uint32_t> reportedEffects_;
    std::unordered_set<uint32_t> reportedDoodads_;
    std::unordered_set<std::string> reportedTextures_;
    bool reportedMissingLayers_ = false;
};

} // namespace rendering
} // namespace wowee
