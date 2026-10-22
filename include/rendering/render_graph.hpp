#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <cstdint>
#include <optional>

namespace wowee::rendering {
class VkContext;

// Logical versions are single-assignment. versionResource() orders the next
// writer after ALL readers of the old version, even if registered out of order.
// Physical resources are imported afresh for the acquired frame slot. The graph
// never owns their allocation, frame fence, queue ownership or retirement.
struct RGResource {
    uint32_t id = UINT32_MAX;
    [[nodiscard]] bool valid() const { return id != UINT32_MAX; }
};
struct RGImageBarrier {
    VkImage image;
    VkImageLayout oldLayout, newLayout;
    VkAccessFlags2 srcAccess, dstAccess;
    VkPipelineStageFlags2 srcStage, dstStage;
    VkImageSubresourceRange range;
};
struct RGBufferBarrier {
    ::VkBuffer buffer;
    VkDeviceSize offset, size;
    VkAccessFlags2 srcAccess, dstAccess;
    VkPipelineStageFlags2 srcStage, dstStage;
};
struct RGState {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct RGUse { RGResource resource; RGState state; };
struct RGPass {
    std::string name;
    std::vector<RGResource> inputs, outputs;
    std::function<void(VkCommandBuffer)> execute;
    bool enabled = true;
    std::vector<RGUse> uses;
    // Compiled Vulkan commands, exposed for capture/debug and CPU validation.
    std::vector<RGImageBarrier> imageBarriers;
    std::vector<RGBufferBarrier> bufferBarriers;
};

class RenderGraph {
public:
    // Clears passes and physical imports; keeps logical resource names/versions.
    void reset();
    RGResource registerResource(const std::string& name);
    RGResource versionResource(RGResource previous, const std::string& name);
    [[nodiscard]] RGResource findResource(const std::string& name) const;

    // Import a root resource after its slot's fence. Images use one declared
    // subresource range; do not import overlapping ranges under different roots.
    // A render pass in a callback must keep the layout declared in use().
    void importImage(RGResource resource, VkImage image, RGState initial,
                     VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1});
    void importBuffer(RGResource resource, ::VkBuffer buffer, RGState initial,
                      VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
    uint32_t addPass(const std::string& name, const std::vector<RGResource>& inputs,
                     const std::vector<RGResource>& outputs,
                     std::function<void(VkCommandBuffer)> execute);
    // Declare every physical resource access of a callback. Legacy passes with
    // logical-only dependencies retain their own explicit barriers.
    void use(uint32_t pass, RGResource resource, RGState state);
    void setPassEnabled(const std::string& name, bool enabled);
    // Invalid graphs log an error and have NO executable order. Never guess an
    // order after a cycle, missing writer, ambiguous version or invalid access.
    bool compile();
    void execute(VkCommandBuffer cmd, VkContext& context, std::string_view stageGroup);
    // Same recording without frame profiling, for offscreen submissions/tools.
    void record(VkCommandBuffer cmd);
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] const std::vector<uint32_t>& getExecutionOrder() const { return executionOrder_; }
    [[nodiscard]] const std::vector<RGPass>& getPasses() const { return passes_; }
private:
    bool fail(std::string message);
    bool topologicalSort();
    bool buildBarriers();
    void recordPass(VkCommandBuffer cmd, const RGPass& pass);
    uint32_t root(uint32_t resource) const;
    struct Import {
        VkImage image = VK_NULL_HANDLE;
        ::VkBuffer buffer = VK_NULL_HANDLE;
        VkImageSubresourceRange range{};
        VkDeviceSize offset = 0, size = VK_WHOLE_SIZE;
        RGState initial;
    };
    struct ResourceEntry {
        std::string name;
        uint32_t previous = UINT32_MAX;
        std::optional<Import> binding;
    };
    std::vector<ResourceEntry> resources_;
    std::vector<RGPass> passes_;
    std::vector<uint32_t> executionOrder_;
    bool compiled_ = false;
    std::string error_, reportedError_;
};
} // namespace wowee::rendering
