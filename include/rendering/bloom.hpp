#pragma once

#include "rendering/vk_utils.hpp"
#include <array>

namespace wowee::rendering {
class VkContext;

/// Soft light around bright scene pixels, before the UI. The current scene
/// targets are LDR: extract highlights before reducing resolution so fine
/// water glints survive, then combine five scales without blurring the scene.
/// Each frame slot owns its images and descriptors; record reads captured
/// settings only. Works with native rendering, MSAA, FXAA and upscaling.
class Bloom {
public:
    ~Bloom();
    bool initialize(VkContext* context);
    void shutdown();
    /// Outside render passes. Source enters and leaves in PRESENT_SRC_KHR.
    bool record(VkCommandBuffer cmd, uint32_t slot, VkImage source,
                VkExtent2D extent, float strength);
    /// Inside the colour-only overlay pass, before the UI.
    void composite(VkCommandBuffer cmd, uint32_t slot);

private:
    static constexpr uint32_t kFrames = 2;
    static constexpr uint32_t kLevels = 5;
    struct Frame {
        AllocatedImage source;
        std::array<AllocatedImage, kLevels> levels;
        std::array<VkDescriptorSet, kLevels> downsample{};
        VkDescriptorSet composite = VK_NULL_HANDLE;
        float strength = 0;
        bool ready = false;
    };
    bool ensureTargets(Frame& frame, VkExtent2D extent);
    bool ensureComposite();
    void retireTargets(Frame& frame);
    VkContext* ctx_ = nullptr;
    bool usable_ = false;
    std::array<Frame, kFrames> frames_;
    VkSampler sampler_ = VK_NULL_HANDLE; // context cache owns it
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout downsampleSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout downsampleLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline downsample_ = VK_NULL_HANDLE;
    VkPipeline composite_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;
};
}
