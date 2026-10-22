#pragma once
#include "rendering/rt_lighting.hpp"
#include "rendering/rt_pass_data.hpp"
#include <array>

namespace wowee::rendering {
// Current evaluated receivers. Scratch is shared on one graphics queue; only
// uniforms and temporal records are per frame slot. No scene traversal here.
class SurfaceRt {
public:
    explicit SurfaceRt(VkContext& context, RtScene& scene);
    ~SurfaceRt();
    void prepare(VkExtent2D extent, VkSampleCountFlagBits samples,
                 VkImageView keys, VkImageView payload, VkDescriptorBufferInfo materials,
                 int mode, const RtLighting::FrameInputs& inputs);
    void record(VkCommandBuffer cmd);
    // Only samples with a nonzero current material key may be read. Completely
    // uncovered tiles are untouched; consumers must check coverage first.
    VkDescriptorBufferInfo output() const;
    const glm::vec3& previousCamera() const { return previousCamera_; }
    bool hasHistory() const { return historyValid_; }
private:
    void createPipelines();
    void resize(VkExtent2D extent, VkSampleCountFlagBits samples);
    void retireBuffers();
    VkContext& ctx_;
    RtScene& scene_;
    VkExtent2D extent_{};
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline,2> base_{}, extra_{}, filter_{};
    std::array<VkDescriptorSet,MAX_FRAMES_IN_FLIGHT> sets_{};
    std::array<AllocatedBuffer,MAX_FRAMES_IN_FLIGHT> uniforms_{}, history_{};
    AllocatedBuffer raw_{}, filtered_{}, queue_{}, indirect_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDeviceSize outputBytes_ = 0, historyBytes_ = 0, tileBytes_ = 0;
    uint32_t sampleCount_ = 1, maxDispatch_ = 1, filtersPerTile_ = 4;
    uint64_t preparedFrame_ = 0, recordedFrame_ = 0;
    glm::mat4 previousViewProj_{1};
    glm::vec3 previousCamera_{0};
    RtPassData frameData_{};
    int previousMode_ = 0;
    float previousDistance_ = 0;
    bool historyValid_ = false;
};
}
