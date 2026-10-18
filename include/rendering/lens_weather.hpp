#pragma once

#include "rendering/frames_in_flight.hpp"
#include "rendering/lens_drops.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>

namespace wowee {
namespace rendering {

class VkContext;

/**
 * The weather on the camera's lens (LensDrops), drawn over the finished
 * picture ahead of the interface: the frost and the dust film over the whole
 * of it, then each drop, flake and grain.
 *
 * A drop shows what is behind it turned over, as a drop of water does, so the
 * frame is copied down to half size first - from the swapchain, where every
 * path this renderer has leaves it, before the interface goes over it (the
 * way SunShafts takes it). Nothing is copied or drawn while the lens is
 * clear. Double-buffered per frame in flight.
 */
class LensWeather {
public:
    LensWeather() = default;
    ~LensWeather();
    LensWeather(const LensWeather&) = delete;
    LensWeather& operator=(const LensWeather&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx);
    void shutdown();

    /// Copy the frame down and upload what is on the lens. Outside any
    /// render pass, after the scene's last one has closed - the image is in
    /// PRESENT_SRC and goes back there. Returns whether composite() draws.
    bool record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage, VkExtent2D extent,
                const LensDrops& lens, glm::vec2 windOnScreen, float time);
    /// Draw it. Inside the overlay pass, viewport set, before the interface.
    void composite(VkCommandBuffer cmd, uint32_t frame);

private:
    struct Slot {
        AllocatedImage copy{};
        AllocatedBuffer drops{};
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t dropCount = 0;
        bool draw = false;
        bool film = false;
        glm::vec4 filmPush{0.0f};
        glm::vec4 windPush{0.0f};
    };

    bool ensureCopies(VkExtent2D extent);
    void destroyCopies();
    bool ensurePipelines();
    void writeSet(uint32_t frame);

    VkContext* ctx_ = nullptr;
    bool usable_ = false;
    std::array<Slot, MAX_FRAMES_IN_FLIGHT> slots_{};
    /// The copy's levels, as many as fit down to a few texels: the lowest
    /// is a twenty-fourth of the frame.
    static constexpr uint32_t kCopyMips = 5;
    uint32_t copyMips_ = 1;
    VkExtent2D copyExtent_{.width = 0, .height = 0};
    VkExtent2D sourceExtent_{.width = 0, .height = 0};
    VkSampler sampler_ = VK_NULL_HANDLE;  // the context's cache owns it

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipeline filmPipeline_ = VK_NULL_HANDLE;
    VkPipeline dropsPipeline_ = VK_NULL_HANDLE;
    /// The swapchain format the pipelines were built for; only a change of
    /// it rebuilds them (see SunShafts::compositeFormat_).
    VkFormat pipelineFormat_ = VK_FORMAT_UNDEFINED;
};

}  // namespace rendering
}  // namespace wowee
