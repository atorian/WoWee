#pragma once
#include "rendering/vk_utils.hpp"
#include "rendering/frames_in_flight.hpp"
#include <glm/glm.hpp>
#include <array>
#include <span>
#include <vector>

namespace wowee::rendering {
class Camera;
class VkContext;
// Stable compaction of a view's captured candidates. A group is shared by all
// compatible material batches; culling never repeats per batch. GPU counts feed
// indexed indirect draws directly, without a host readback or an extra submit.
class GpuDrawList {
public:
    explicit GpuDrawList(VkContext& context);
    ~GpuDrawList();
    GpuDrawList(const GpuDrawList&)=delete;
    GpuDrawList& operator=(const GpuDrawList&)=delete;
    void prepare(uint32_t slot,size_t candidateCapacity,size_t drawCapacity);
    uint32_t candidateCount() const { return uint32_t(spheres_.size()); }
    uint32_t appendSphere(glm::vec4 sphere);
    uint32_t addGroup(uint32_t first,uint32_t count);
    VkDeviceSize addDraw(uint32_t group,uint32_t indexCount,uint32_t firstIndex,int32_t vertexOffset=0);
    bool hasDraws() const { return !draws_.empty(); }
    VkBuffer visibleBuffer() const;
    VkBuffer drawBuffer() const;
    void record(VkCommandBuffer command,const Camera& camera,bool forceVisible);
private:
    struct Group { uint32_t first,count,visible; };
    struct Draw {
        VkDrawIndexedIndirectCommand command;
        uint32_t group;
    };
    struct Buffer {
        AllocatedBuffer gpu;
        size_t bytes=0;
        std::vector<std::byte> uploaded;
    };
    struct Slot {
        std::array<Buffer,4> buffers;
        VkDescriptorSet set=VK_NULL_HANDLE;
    };
    void reserve(Buffer& buffer,size_t bytes,bool host,VkBufferUsageFlags usage);
    void upload(Buffer& buffer,const void* values,size_t count,size_t stride);
    VkContext& ctx_;
    VkDescriptorSetLayout setLayout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    std::array<Slot,MAX_FRAMES_IN_FLIGHT> slots_;
    uint32_t slot_=0;
    size_t candidateCapacity_=0,drawCapacity_=0;
    std::vector<glm::vec4> spheres_;
    std::vector<Group> groups_;
    std::vector<Draw> draws_;
};
}
