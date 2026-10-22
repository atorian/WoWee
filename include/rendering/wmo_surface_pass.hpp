#pragma once
#include "rendering/vk_utils.hpp"
#include "rendering/frames_in_flight.hpp"
#include <glm/glm.hpp>
#include <array>
#include <vector>

namespace wowee::rendering {
class VkContext;
// Transient evaluated surfaces, shared on the graphics queue. Each frame slot
// owns its material upload. No scene access or worker waits during recording.
class WmoSurfacePass {
public:
    struct Material { std::array<uint32_t,16> words; glm::uvec4 localLights; };
    struct TargetVersion {
        uint64_t swapchain, postProcess;
        bool offscreen;
        bool operator==(const TargetVersion&) const = default;
    };
    WmoSurfacePass(VkContext& ctx, VkDescriptorSetLayout frameLayout);
    ~WmoSurfacePass();
    void prepare(VkExtent2D extent, VkImageView depth, TargetVersion version, size_t materialCapacity);
    uint32_t appendMaterial(const Material& material);
    void finishMaterials();
    VkRenderPass geometryPass() const { return pass_; }
    // prepare runs on the render owner after the slot fence; one worker then
    // records contiguous authored runs. execute only walks these commands.
    VkCommandBuffer beginRun(bool geometry);
    void endRun();
    void execute(VkCommandBuffer cmd, VkDescriptorSet frameSet, VkFramebuffer scene,
                 const glm::mat4& inverseViewProjection, VkSubpassContents resumeContents);
private:
    void beginGeometry(VkCommandBuffer cmd);
    void light(VkCommandBuffer cmd, VkDescriptorSet frameSet, VkFramebuffer scene,
               const glm::mat4& inverseViewProjection);
    void resumeScene(VkCommandBuffer cmd, VkFramebuffer scene, VkSubpassContents contents);
    void retire();
    void create(VkExtent2D extent, VkImageView depth);
    VkContext& ctx_;
    VkDescriptorSetLayout frameLayout_;
    VkExtent2D extent_{};
    VkImageView depth_ = VK_NULL_HANDLE;
    TargetVersion targetVersion_{};
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED, colorFormat_ = VK_FORMAT_UNDEFINED;
    std::array<AllocatedImage,3> images_{};
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    static constexpr size_t kSlots = MAX_FRAMES_IN_FLIGHT;
    std::array<AllocatedBuffer,kSlots> buffers_{};
    std::array<size_t,kSlots> capacities_{};
    std::array<VkDescriptorSet,kSlots> sets_{};
    struct Run { VkCommandBuffer command; bool geometry; };
    struct Commands {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
        std::vector<Run> runs;
    };
    std::array<Commands,kSlots> commands_{};
    size_t materialLimit_ = 0, materialsWritten_ = 0;
};
}
