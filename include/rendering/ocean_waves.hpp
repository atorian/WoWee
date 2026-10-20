#pragma once

#include "rendering/ocean_sea.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <array>

namespace wowee {
namespace rendering {

class VkContext;

/// How many patches the sea is drawn in: kOceanCascades in
/// ocean_spectrum.glsli, which ocean_waves.cpp checks this against.
inline constexpr int kOceanCascadeCount = 3;

/**
 * The wind's sea on the GPU, Tessendorf's way: a spectrum of waves on a grid
 * of wave vectors, turned into the surface by an inverse FFT every frame.
 *
 * Three cascades, each a 256 x 256 grid of wave vectors repeating over its
 * own patch - 251, 31.7 and 4.3 yards, ratios that never line their repeats
 * up - and each holding its own band of wavelengths, so together they run
 * from the patch-long swell down to a hand's breadth (ocean_spectrum.glsli).
 * Five compute passes a frame, outside any render pass:
 *
 *   spectrum   each wave vector's height variance one step on toward what
 *              the wind asks of it
 *   evolve     its fields now, packed two real fields to a complex number
 *   fft        rows, then columns, through the inverse FFT in place
 *   assemble   the surface the water samples: where each point of the still
 *              water has gone, the derivatives that shape its light, and
 *              their squares
 *
 * and the surface's mips filtered down, so the mesh takes the waves its step
 * can carry and a pixel the slope it can show, and the rest of the slope it
 * covers from the squares (LEAN mapping).
 *
 * One surface image, read by the water of both frames in flight: every pass
 * waits on the reads of the frame before on the same queue.
 */
class OceanWaves {
public:
    OceanWaves() = default;
    ~OceanWaves();
    OceanWaves(const OceanWaves&) = delete;
    OceanWaves& operator=(const OceanWaves&) = delete;

    /// The images and passes. The surface image is made, flat, even where
    /// the passes cannot be, so the water always has one to sample.
    [[nodiscard]] bool initialize(VkContext* ctx);
    void shutdown();

    /// Move the sea on by `deltaSeconds` under a wind of `speedYardsPerSecond`
    /// (gust in) blowing toward `windDirection`, its patches placed near
    /// `nearby` (the player). Once per frame.
    void advance(double deltaSeconds, float speedYardsPerSecond, glm::vec2 windDirection, glm::vec2 nearby);

    /// Build this frame's surface. Outside any render pass, before the water
    /// is drawn.
    struct Frame {
        glm::vec2 toward{1.0f, 0.0f};
        float speed = 0, slopeScale = 0, time = 0, ride = 0;
        double elapsed = 0;
    };
    Frame extract();
    void record(VkCommandBuffer cmd, const Frame& frame);

    /// The surface, all its layers and mips, for a sampler2DArray, and the
    /// repeating, filtering sampler to read it with.
    [[nodiscard]] VkImageView surfaceView() const { return surfaceSampledView_; }
    [[nodiscard]] VkSampler surfaceSampler() const { return sampler_; }

    /// Per cascade, for the water's shaders: xy = an origin near the player a
    /// whole number of patches from the world's, z = 1 / the patch in yards,
    /// w = the patch.
    [[nodiscard]] const std::array<glm::vec4, kOceanCascadeCount>& cascades() const { return cascades_; }
    /// The sea as it stands: x = its height's root mean square in yards,
    /// y = how far its water rides toward the crests, z = its mean square
    /// slope.
    [[nodiscard]] glm::vec4 seaState() const;
    /// The sea's totals as the CPU keeps them.
    [[nodiscard]] const OceanSea& sea() const { return sea_; }

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool createImage(Image& out, VkFormat format, uint32_t layers, uint32_t mips,
                     VkImageUsageFlags usage, const char* name);
    void destroyImage(Image& image);
    bool createPasses();
    void recordMips(VkCommandBuffer cmd);

    VkContext* ctx_ = nullptr;
    OceanSea sea_;
    std::array<glm::vec4, kOceanCascadeCount> cascades_{};
    double extractedSeconds_ = 0, recordedSeconds_ = 0;
    double pendingSeconds_ = 0.0;   // the sea's time the GPU has not stepped yet
    bool restartPending_ = true;    // the GPU's spectrum has not been set yet

    Image state_;     // r32f, a layer a cascade
    Image modes_;     // rgba32f, two layers a cascade
    Image surface_;   // rgba16f, three layers a cascade, with mips
    VkImageView surfaceSampledView_ = VK_NULL_HANDLE;
    uint32_t surfaceMips_ = 1;
    VkSampler sampler_ = VK_NULL_HANDLE;  // owned by the context's cache

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipeline spectrumPipeline_ = VK_NULL_HANDLE;
    VkPipeline evolvePipeline_ = VK_NULL_HANDLE;
    VkPipeline fftPipeline_ = VK_NULL_HANDLE;
    VkPipeline assemblePipeline_ = VK_NULL_HANDLE;
    bool passesReady_ = false;
};

} // namespace rendering
} // namespace wowee
