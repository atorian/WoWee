#pragma once

#include <cstdint>
#include "rendering/rt_frame_inputs.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "rendering/vk_context.hpp"
#include "rendering/render_graph.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

class RtScene;

// Historical depth-reconstructed RT implementation, linked only by rt_probe.
// The product renderer uses SurfaceRt and explicit current forward receivers.
class RtLighting {
public:
    /// Persisted as an index; add at the end.
    enum class Mode : int { Off = 0, Shadows = 1, ShadowsAO = 2, Full = 3 };

    RtLighting() = default;
    ~RtLighting();
    RtLighting(const RtLighting&) = delete;
    RtLighting& operator=(const RtLighting&) = delete;

    /// Creates the sampler and the neutral image the per-frame sets bind while
    /// the pass is off. Pipelines are built on first use.
    bool initialize(VkContext* ctx, RtScene* scene);
    void shutdown();

    void setMode(Mode mode) {
        if (mode_ == mode) return;
        mode_ = mode;
        // A different mode changes which channels are written. Never blend
        // stale GI history after returning from shadows-only or AO mode.
        havePrev_ = false;
        resultFrame_ = 0;
    }
    void setDistance(float distance) {
        if (distance_ == distance) return;
        distance_ = distance;
        havePrev_ = false;
        resultFrame_ = 0;
    }
    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool active() const { return mode_ != Mode::Off && pipelinesOk_; }

    /// Bring the images to the size the scene is rendered at. Call between
    /// frames, before the per-frame sets are used. Returns true when the
    /// views changed and the per-frame sets must be written again.
    bool prepare(VkExtent2D sceneExtent);

    /// The immutable sampler for per-frame lighting, reflection and geometry bindings.
    [[nodiscard]] VkSampler sampler() const { return linearSampler_; }
    [[nodiscard]] VkImage reflectionImageWrittenBy(uint32_t slot) const { return slots_[slot].reflection.img.image; }
    [[nodiscard]] VkImageView neutralView() const { return neutral_.imageView; }
    /// The images slot `slot` writes, in GENERAL layout, at the trace size.
    /// For tools that read the result back (tools/rt_probe.cpp).
    [[nodiscard]] VkImage lightImageWrittenBy(uint32_t slot) const { return slots_[slot].out.img.image; }
    [[nodiscard]] VkImage giImageWrittenBy(uint32_t slot) const { return slots_[slot].outGi.img.image; }
    [[nodiscard]] VkExtent2D traceExtent() const { return traceExtent_; }

    using FrameInputs = RtFrameInputs;

    /// Record the pass. Outside a render pass, after the opaque geometry.
    /// The depth image must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL and is
    /// returned to it.
    void record(VkCommandBuffer cmd, VkImage sceneDepth, VkExtent2D sceneExtent, bool depthMsaa,
                const FrameInputs& in, bool sceneAlreadyPrepared = false);


private:
    struct Target {
        AllocatedImage img{};
    };
    struct Slot {
        Target gbuf, hist, histGi, out, outGi, reflection;
        AllocatedBuffer ubo{};
        VkDescriptorSet sets[3] = {};  // one per filter pass; set 0 also runs the rest
        VkImage boundDepth = VK_NULL_HANDLE;
        bool wroteImages = false;
    };

    bool createPipelines();
    bool createImages(VkExtent2D traceExtent);
    void destroyImages();
    void writeSlotSets(uint32_t slot);
    VkImageView depthViewFor(VkImage image, bool msaa);

    RenderGraph graph_;
    VkContext* ctx_ = nullptr;
    RtScene* scene_ = nullptr;
    Mode mode_ = Mode::Off;
    bool pipelinesTried_ = false;
    bool pipelinesOk_ = false;

    VkExtent2D sceneExtent_{};
    VkExtent2D traceExtent_{};
    Slot slots_[MAX_FRAMES_IN_FLIGHT];
    Target raw_, rawGi_, tmp_, tmpGi_;
    AllocatedImage neutral_{};

    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkSampler pointSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline prepare_ = VK_NULL_HANDLE;
    VkPipeline prepareMs_ = VK_NULL_HANDLE;
    VkPipeline trace_ = VK_NULL_HANDLE;
    VkPipeline temporal_ = VK_NULL_HANDLE;
    VkPipeline filter_ = VK_NULL_HANDLE;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    bool depthViewMsaa_ = false;

    // Counted by prepare(); a result is usable only in the frame right after
    // the one that traced it, since that is whose slot the sets bind.
    uint64_t frameCounter_ = 0;
    uint64_t resultFrame_ = 0;
    Mode resultMode_ = Mode::Off;
    float distance_ = 0;
    glm::mat4 prevViewProj_{1.0f};
    glm::vec3 prevCameraPos_{0.0f};
    bool havePrev_ = false;
};

} // namespace rendering
} // namespace wowee
