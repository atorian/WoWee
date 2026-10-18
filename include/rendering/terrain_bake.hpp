#pragma once

/**
 * terrain_bake.hpp - the far ground, its layers blended once.
 *
 * Most of the ground on screen is far away, and there a chunk's pixel shader
 * still read four layer textures, three alpha maps and four relief maps for
 * every pixel: in Dalaran that was some seven of the terrain pass's eleven
 * milliseconds, beyond 800 yards. Far enough out, the layers are read at a mip
 * where each texel covers a yard or more of ground, and blending them there is
 * blending the same few colours every frame.
 *
 * So each tile keeps an atlas: 32x32 texels a chunk, the chunk's colour and
 * its relief as terrain.frag blends them (terrain_layers.glsli), drawn once
 * when the tile arrives. A chunk whose texels on screen are no smaller than a
 * baked texel reads the atlas instead of its layers: what its layers would
 * have given there is what the bake already holds, at the mip a pixel that
 * size reads. Between the two a band crossfades, so no edge is drawn where a
 * chunk changes over.
 */

#include "rendering/vk_utils.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace wowee {
namespace rendering {

class VkContext;

/// Texels a chunk is baked at, a side, and so a tile's atlas, a side.
inline constexpr uint32_t kTerrainBakeTexelsPerChunk = 32;
inline constexpr uint32_t kTerrainBakeAtlasSize = 16 * kTerrainBakeTexelsPerChunk;

/// How a chunk's surface is read for where it is.
enum class TerrainSurfaceSource : uint8_t {
    Layers = 0,     ///< its layers, all of it nearer than the band
    Crossfade = 1,  ///< both, across the band
    Baked = 2,      ///< the atlas, all of it beyond the band
};

/// The distances, in yards from the camera, over which the bake takes over: it
/// starts where a baked texel - a chunk over 32 - is one pixel on screen
/// (`focalPixels` the vertical focal length), and has the whole chunk a
/// quarter further on.
struct TerrainBakeBand {
    float start = 0.0f;
    float end = 0.0f;
};
[[nodiscard]] inline TerrainBakeBand terrainBakeBand(float chunkSizeYards, float focalPixels) {
    // A baked texel is the chunk over 32; it is a pixel wide on screen where
    // the distance is that many yards per pixel times the focal length.
    constexpr float kBandWidth = 0.25f;
    const float texel = chunkSizeYards / static_cast<float>(kTerrainBakeTexelsPerChunk);
    const float start = texel * focalPixels;
    return {start, start * (1.0f + kBandWidth)};
}

/// Which a chunk reads whose points lie between `nearest` and `farthest`
/// yards from the camera, and whose steepest ground has a normal `steepestUpZ`
/// up (1 flat, 0 a wall).
///
/// The layers are laid over the map from above, so on a slope a yard of
/// ground covers less of them: across a slope with up-component z, a pixel
/// reads the layers as finely as on flat ground z times nearer, and the bake -
/// laid the same way - holds them only as finely as flat ground needs. A
/// chunk is baked whole only where its steepest point is past the band by
/// that measure, and every pixel crossfades by its own (terrain.frag).
[[nodiscard]] inline TerrainSurfaceSource terrainSurfaceSourceFor(const TerrainBakeBand& band,
                                                                 float nearest, float farthest,
                                                                 float steepestUpZ) {
    const float upZ = std::clamp(steepestUpZ, 0.0f, 1.0f);
    if (nearest * upZ >= band.end) return TerrainSurfaceSource::Baked;
    // Flat ground is the first to reach the band.
    if (farthest <= band.start) return TerrainSurfaceSource::Layers;
    return TerrainSurfaceSource::Crossfade;
}

/// The atlases, the pipeline that draws into them, and the recording.
class TerrainBake {
public:
    struct Atlas {
        AllocatedImage color;      ///< every mip, sampled by the ground
        AllocatedImage relief;
        VkImageView colorTarget = VK_NULL_HANDLE;   ///< mip 0, drawn into
        VkImageView reliefTarget = VK_NULL_HANDLE;
        bool ready = false;        ///< cleared and in the layout the ground samples
    };

    TerrainBake() = default;
    ~TerrainBake();
    TerrainBake(const TerrainBake&) = delete;
    TerrainBake& operator=(const TerrainBake&) = delete;

    /// The pipeline, with the chunks' material set at set 0. False when the
    /// far ground cannot be baked here - said why, once - and the ground is
    /// then drawn from its layers however far.
    bool initialize(VkContext& ctx, VkDescriptorSetLayout materialSetLayout);
    void shutdown();
    [[nodiscard]] bool enabled() const { return pipeline_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }

    /// The tile's atlas, made the first time it is asked for; null where it
    /// could not be (said once a tile).
    Atlas* atlas(int tileX, int tileY);
    /// Gone after the frames that may still sample it.
    void releaseTile(int tileX, int tileY);
    void releaseAll();
    [[nodiscard]] size_t atlasCount() const { return atlases_.size(); }
    [[nodiscard]] uint64_t atlasBytes() const;

    /// Every atlas made since the last call cleared and turned to the layout
    /// the ground samples, before any chunk names it in a draw.
    void prepareNewAtlases(VkCommandBuffer cmd);

    /// Drawing a tile's chunks into its atlas: begin, a cell and its draws per
    /// chunk, end - which makes the mips and leaves the atlas for sampling.
    void beginTile(VkCommandBuffer cmd, Atlas& atlas);
    void cell(VkCommandBuffer cmd, int chunkX, int chunkY);
    void endTile(VkCommandBuffer cmd, Atlas& atlas);
    [[nodiscard]] VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }

private:
    static uint64_t tileKey(int tileX, int tileY) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(tileX)) << 32) |
               static_cast<uint32_t>(tileY);
    }
    void destroyAtlas(Atlas& atlas);

    VkContext* ctx_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t mipLevels_ = 1;
    std::unordered_map<uint64_t, std::unique_ptr<Atlas>> atlases_;
    bool atlasFailureSaid_ = false;
};

}  // namespace rendering
}  // namespace wowee
