#include "rendering/gpu_draw_list.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wowee::rendering {
namespace {
void requireDraw(bool condition,const char* error) {
    if(condition)return;
    LOG_ERROR("GPU draw list: ",error,"; frame cannot be rendered");
    throw std::runtime_error(error);
}
struct Push {
    glm::vec4 planes[6];
    glm::uvec4 work; // count, first group/invocation, phase, force-visible
};
}
GpuDrawList::GpuDrawList(VkContext& context):ctx_(context) {
    const auto device=ctx_.getDevice();
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(uint32_t i=0;i<4;++i)
        bindings.push_back({i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    setLayout_=createDescriptorSetLayout(device,bindings);
    requireDraw(setLayout_!=VK_NULL_HANDLE,"descriptor layout creation failed");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4*MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=MAX_FRAMES_IN_FLIGHT;pool.poolSizeCount=1;pool.pPoolSizes=&size;
    requireDraw(vkCreateDescriptorPool(device,&pool,nullptr,&pool_)==VK_SUCCESS,"descriptor pool creation failed");
    for(auto& slot:slots_) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=pool_;allocate.descriptorSetCount=1;allocate.pSetLayouts=&setLayout_;
        requireDraw(vkAllocateDescriptorSets(device,&allocate,&slot.set)==VK_SUCCESS,"descriptor allocation failed");
    }
    layout_=createPipelineLayout(device,{setLayout_},{{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)}});
    requireDraw(layout_!=VK_NULL_HANDLE,"pipeline layout creation failed");
    VkShaderModule shader;
    requireDraw(shader.loadFromFile(device,"assets/shaders/gpu_draw_list.comp.spv"),"compaction shader unavailable");
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute.layout=layout_;compute.stage=shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    const auto result=vkCreateComputePipelines(device,ctx_.getPipelineCache(),1,&compute,nullptr,&pipeline_);
    shader.destroy();requireDraw(result==VK_SUCCESS,"compaction pipeline creation failed");
}
GpuDrawList::~GpuDrawList() {
    DeferredResources retire(ctx_);
    retire.destroy(pipeline_);retire.destroy(layout_);retire.destroy(pool_);retire.destroy(setLayout_);
    for(auto& slot:slots_)for(auto& buffer:slot.buffers)retire.buffer(buffer.gpu);
}
void GpuDrawList::reserve(Buffer& buffer,size_t bytes,bool host,VkBufferUsageFlags usage) {
    bytes=std::max(size_t(16),bytes);
    if(buffer.bytes>=bytes && (buffer.bytes<=65536 || bytes>=buffer.bytes/3))return;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(ctx_.getPhysicalDevice(),&properties);
    const size_t capacity=std::min(size_t(properties.limits.maxStorageBufferRange),
        std::max(bytes,buffer.bytes<bytes?buffer.bytes*2:bytes));
    requireDraw(capacity>=bytes,"storage buffer range exceeded");
    auto next=createBuffer(ctx_.getAllocator(),capacity,usage|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        host?VMA_MEMORY_USAGE_CPU_TO_GPU:VMA_MEMORY_USAGE_GPU_ONLY);
    requireDraw(next.buffer!=VK_NULL_HANDLE && (!host || next.info.pMappedData),"buffer allocation failed");
    DeferredResources retire(ctx_);retire.buffer(buffer.gpu);
    buffer.gpu=next;buffer.bytes=capacity;buffer.uploaded.clear();
}
void GpuDrawList::prepare(uint32_t slot,size_t candidateCapacity,size_t drawCapacity) {
    slot_=slot;candidateCapacity_=candidateCapacity;drawCapacity_=drawCapacity;
    spheres_.clear();groups_.clear();draws_.clear();
    spheres_.reserve(candidateCapacity);groups_.reserve(candidateCapacity);draws_.reserve(drawCapacity);
    auto& state=slots_[slot];
    reserve(state.buffers[0],candidateCapacity*sizeof(glm::vec4),true,0);
    reserve(state.buffers[1],candidateCapacity*sizeof(Group),true,0);
    reserve(state.buffers[2],candidateCapacity*sizeof(uint32_t),false,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    reserve(state.buffers[3],drawCapacity*sizeof(Draw),true,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkDescriptorBufferInfo buffers[4]{};VkWriteDescriptorSet writes[4]{};
    for(uint32_t i=0;i<4;++i) {
        buffers[i]={state.buffers[i].gpu.buffer,0,state.buffers[i].bytes};
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=state.set;
        writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo=&buffers[i];
    }
    vkUpdateDescriptorSets(ctx_.getDevice(),4,writes,0,nullptr);
}
uint32_t GpuDrawList::appendSphere(glm::vec4 sphere) {
    requireDraw(spheres_.size()<candidateCapacity_,"candidate reservation exhausted");
    const auto index=uint32_t(spheres_.size());spheres_.push_back(sphere);return index;
}
uint32_t GpuDrawList::addGroup(uint32_t first,uint32_t count) {
    requireDraw(groups_.size()<candidateCapacity_ && uint64_t(first)+count<=spheres_.size() &&
        (groups_.empty() || first>=groups_.back().first+groups_.back().count),"invalid or overlapping candidate group");
    const auto index=uint32_t(groups_.size());groups_.push_back({first,count,0});return index;
}
VkDeviceSize GpuDrawList::addDraw(uint32_t group,uint32_t indexCount,uint32_t firstIndex,int32_t vertexOffset) {
    requireDraw(draws_.size()<drawCapacity_ && group<groups_.size(),"draw reservation exhausted");
    const auto offset=VkDeviceSize(draws_.size())*sizeof(Draw);
    draws_.push_back({{indexCount,0,firstIndex,vertexOffset,0},group});return offset;
}
VkBuffer GpuDrawList::visibleBuffer() const { return slots_[slot_].buffers[2].gpu.buffer; }
VkBuffer GpuDrawList::drawBuffer() const { return slots_[slot_].buffers[3].gpu.buffer; }
void GpuDrawList::upload(Buffer& buffer,const void* values,size_t count,size_t stride) {
    const auto* source=static_cast<const std::byte*>(values);
    const size_t bytes=count*stride,old=buffer.uploaded.size();
    requireDraw(bytes<=buffer.bytes,"upload exceeds reservation");
    buffer.uploaded.resize(bytes);
    size_t first=bytes,last=0;
    for(size_t offset=0;offset<bytes;offset+=stride) {
        if(offset+stride<=old && !std::memcmp(buffer.uploaded.data()+offset,source+offset,stride))continue;
        std::memcpy(buffer.uploaded.data()+offset,source+offset,stride);
        std::memcpy(static_cast<std::byte*>(buffer.gpu.info.pMappedData)+offset,source+offset,stride);
        first=std::min(first,offset);last=offset+stride;
    }
    if(last)requireDraw(vmaFlushAllocation(ctx_.getAllocator(),buffer.gpu.allocation,first,last-first)==VK_SUCCESS,
        "upload flush failed");
}
void GpuDrawList::record(VkCommandBuffer command,const Camera& camera,bool forceVisible) {
    if(draws_.empty())return;
    if(core::envFlagEnabled("WOWEE_SURFACE_PLAN_CHECK",false))
        LOG_INFO("GPU draw list: candidates=",spheres_.size()," groups=",groups_.size()," draws=",draws_.size());
    auto& state=slots_[slot_];
    upload(state.buffers[0],spheres_.data(),spheres_.size(),sizeof(glm::vec4));
    upload(state.buffers[1],groups_.data(),groups_.size(),sizeof(Group));
    upload(state.buffers[3],draws_.data(),draws_.size(),sizeof(Draw));
    Frustum frustum;frustum.extractFromMatrix(camera.getViewProjectionMatrix());
    Push push{};
    for(int i=0;i<6;++i) {
        const auto& plane=frustum.getPlane(static_cast<Frustum::Side>(i));
        push.planes[i]=glm::vec4(plane.normal,plane.distance);
    }
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&state.set,0,nullptr);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(ctx_.getPhysicalDevice(),&properties);
    for(uint32_t phase=0;phase<2;++phase) {
        const uint32_t count=uint32_t(phase?draws_.size():groups_.size());
        const uint32_t workgroups=phase?(count+63)/64:count;
        for(uint32_t first=0;first<workgroups;) {
            const uint32_t batch=std::min(workgroups-first,properties.limits.maxComputeWorkGroupCount[0]);
            push.work={count,phase?first*64:first,phase,forceVisible?1u:0u};
            vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
            vkCmdDispatch(command,batch,1,1);first+=batch;
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask=phase?VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            phase?VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,1,&barrier,0,nullptr,0,nullptr);
    }
}
}
