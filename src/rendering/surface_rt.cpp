#include "rendering/surface_rt.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/rt_region.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include "core/env_flag.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

namespace wowee::rendering {
namespace {
void requireRt(bool success, const char* operation) {
    if (success) return;
    LOG_ERROR("Current surface RT: ", operation, " - frame cannot be rendered");
    throw std::runtime_error(operation);
}
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 source, VkAccessFlags2 sourceAccess,
             VkPipelineStageFlags2 destination, VkAccessFlags2 destinationAccess) {
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask=source; memory.srcAccessMask=sourceAccess;
    memory.dstStageMask=destination; memory.dstAccessMask=destinationAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount=1; dependency.pMemoryBarriers=&memory;
    cmdPipelineBarrier2(cmd,dependency);
}
}
SurfaceRt::SurfaceRt(VkContext& context,RtScene& scene):ctx_(context),scene_(scene) { createPipelines(); }
SurfaceRt::~SurfaceRt() {
    retireBuffers();
    DeferredResources release(ctx_);
    for(auto* pipelines:{&base_,&extra_,&filter_}) for(auto& pipeline:*pipelines)release.destroy(pipeline);
    release.destroy(layout_);release.destroy(pool_);release.destroy(setLayout_);
    for(auto& uniform:uniforms_)release.buffer(uniform);
}
void SurfaceRt::retireBuffers() {
    DeferredResources release(ctx_);
    for(auto* buffer:{&raw_,&filtered_,&queue_,&indirect_})release.buffer(*buffer);
    for(auto& history:history_)release.buffer(history);
    recordedFrame_=0;
}
void SurfaceRt::createPipelines() {
    const auto device=ctx_.getDevice();
    std::array<VkDescriptorSetLayoutBinding,10> bindings{};
    for(uint32_t i=0;i<bindings.size();++i) {
        auto type=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            i<3?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i]={i,type,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    }
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount=bindings.size();setInfo.pBindings=bindings.data();
    requireRt(vkCreateDescriptorSetLayout(device,&setInfo,nullptr,&setLayout_)==VK_SUCCESS,"descriptor layout creation failed");
    constexpr uint32_t slots=MAX_FRAMES_IN_FLIGHT;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,slots},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*slots},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,7*slots}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount=3;poolInfo.pPoolSizes=sizes;poolInfo.maxSets=slots;
    requireRt(vkCreateDescriptorPool(device,&poolInfo,nullptr,&pool_)==VK_SUCCESS,"descriptor pool creation failed");
    for(uint32_t i=0;i<slots;++i) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool=pool_;info.descriptorSetCount=1;info.pSetLayouts=&setLayout_;
        requireRt(vkAllocateDescriptorSets(device,&info,&sets_[i])==VK_SUCCESS,"descriptor allocation failed");
        uniforms_[i]=createBuffer(ctx_.getAllocator(),sizeof(RtPassData),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
        requireRt(uniforms_[i].info.pMappedData!=nullptr,"uniform allocation failed");
    }
    VkDescriptorSetLayout layouts[]={setLayout_,scene_.descriptorSetLayout()};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,12};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount=2;info.pSetLayouts=layouts;info.pushConstantRangeCount=1;info.pPushConstantRanges=&push;
    requireRt(vkCreatePipelineLayout(device,&info,nullptr,&layout_)==VK_SUCCESS,"pipeline layout creation failed");
    auto pipeline=[&](const std::string& name,VkPipeline& output) {
        VkShaderModule shader;
        requireRt(shader.loadFromFile(device,"assets/shaders/"+name+".comp.spv"),"compute shader unavailable");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage=shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);ci.layout=layout_;
        const auto result=vkCreateComputePipelines(device,ctx_.getPipelineCache(),1,&ci,nullptr,&output);
        shader.destroy();requireRt(result==VK_SUCCESS,"compute pipeline creation failed");
    };
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(ctx_.getPhysicalDevice(),&properties);
    // Conservative vec3 alignment: 576 * (vec4 + vec3 + uint + uvec2).
    // Also check the invocation limit before selecting the larger workgroup.
    const bool wide=properties.limits.maxComputeSharedMemorySize>=576u*44u &&
        properties.limits.maxComputeWorkGroupInvocations>=256 &&
        properties.limits.maxComputeWorkGroupSize[0]>=16 && properties.limits.maxComputeWorkGroupSize[1]>=16 &&
        !core::envFlagEnabled("WOWEE_SURFACE_RT_FILTER_8",false);
    filtersPerTile_=wide?1u:4u;
    const std::string hardware=scene_.backend()==RtScene::Backend::Hardware?"_hw":"";
    for(uint32_t ms=0;ms<2;++ms) {
        const std::string suffix=ms?"_ms":"";
        pipeline("surface_rt"+hardware+suffix,base_[ms]);
        pipeline("surface_rt_extra"+hardware+suffix,extra_[ms]);
        pipeline(std::string("surface_rt_filter")+(wide?"_wide":"")+suffix,filter_[ms]);
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_=ctx_.getOrCreateSampler(sampler);
}
void SurfaceRt::resize(VkExtent2D extent,VkSampleCountFlagBits samples) {
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(ctx_.getPhysicalDevice(),&properties);
    const uint64_t count=uint64_t(extent.width)*extent.height*uint32_t(samples);
    const uint64_t blocks=uint64_t((extent.width+1)/2)*((extent.height+1)/2);
    requireRt(count<=UINT32_MAX && count*8<=properties.limits.maxStorageBufferRange &&
              blocks*40<=properties.limits.maxStorageBufferRange,"receiver/history table exceeds device storage range");
    requireRt(properties.limits.maxPerStageDescriptorStorageBuffers>=7,"seven compute storage buffers unsupported");
    retireBuffers();extent_=extent;samples_=samples;sampleCount_=uint32_t(samples);
    maxDispatch_=properties.limits.maxComputeWorkGroupCount[0];
    outputBytes_=count*8;historyBytes_=blocks*40;
    tileBytes_=uint64_t((extent.width+15)/16)*((extent.height+15)/16)*8;
    auto allocate=[&](AllocatedBuffer& buffer,VkDeviceSize bytes,VkBufferUsageFlags additional=0) {
        buffer=createBuffer(ctx_.getAllocator(),bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|additional,VMA_MEMORY_USAGE_GPU_ONLY);
        requireRt(buffer.buffer!=VK_NULL_HANDLE,"receiver buffer allocation failed");
    };
    allocate(raw_,outputBytes_);allocate(filtered_,outputBytes_,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);allocate(queue_,tileBytes_);
    allocate(indirect_,48,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    for(auto& history:history_)allocate(history,historyBytes_);
    LOG_INFO("Current surface RT: ",extent.width,"x",extent.height," samples=",sampleCount_,
        " scratch bytes=",count*16+tileBytes_+48," history bytes=",historyBytes_*MAX_FRAMES_IN_FLIGHT,
        " filter tile=",filtersPerTile_==1?16:8," backend=",scene_.backend()==RtScene::Backend::Hardware?"hardware":"software");
}
void SurfaceRt::prepare(VkExtent2D extent,VkSampleCountFlagBits samples,VkImageView keys,VkImageView payload,
        VkDescriptorBufferInfo materials,int mode,const RtLighting::FrameInputs& in) {
    if(extent.width!=extent_.width || extent.height!=extent_.height || samples!=samples_)resize(extent,samples);
    preparedFrame_=ctx_.frameNumber();
    historyValid_=recordedFrame_!=0 && recordedFrame_+1==preparedFrame_ && previousMode_==mode && previousDistance_==in.distance;
    const auto slot=ctx_.getCurrentFrame(), previous=(slot+MAX_FRAMES_IN_FLIGHT-1)%MAX_FRAMES_IN_FLIGHT;
    const auto relative=in.projection*glm::mat4(glm::mat3(in.view));
    frameData_={};auto& d=frameData_;
    d.invViewProj=glm::inverse(relative);d.prevViewProj=historyValid_?previousViewProj_:relative;
    d.cameraPos=glm::vec4(in.cameraPos,float(preparedFrame_&0xffffff));
    d.prevCameraPos=glm::vec4(historyValid_?previousCamera_:in.cameraPos,0);
    d.sunDir=glm::vec4(glm::normalize(in.sunDir),kRtSunTanRadius);
    d.sunColor=glm::vec4(in.sunColor,in.sunUp?1.f:0.f);d.skyColor=glm::vec4(in.skyColor,0);
    d.params=glm::vec4(mode,kRtAoRadius,kRtGiLength,kRtShadowLength);
    const float width=(extent.width+1)/2,height=(extent.height+1)/2;
    d.extent={width,height,1/width,1/height};
    d.sceneExtent={extent.width,extent.height,1.f/extent.width,1.f/extent.height};
    d.misc={scene_.hasContent()?1.f:0.f,historyValid_?1.f:0.f,0,in.distance};
    std::memcpy(uniforms_[slot].info.pMappedData,&d,sizeof(d));
    requireRt(vmaFlushAllocation(ctx_.getAllocator(),uniforms_[slot].allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"uniform flush failed");
    VkDescriptorBufferInfo buffers[10]{};
    buffers[0]={uniforms_[slot].buffer,0,sizeof(d)};
    buffers[3]={raw_.buffer,0,outputBytes_};buffers[4]={filtered_.buffer,0,outputBytes_};
    buffers[5]={queue_.buffer,0,tileBytes_};
    buffers[6]={indirect_.buffer,0,48};buffers[7]=materials;
    buffers[8]={history_[previous].buffer,0,historyBytes_};buffers[9]={history_[slot].buffer,0,historyBytes_};
    VkDescriptorImageInfo images[]={{sampler_,keys,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_,payload,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::array<VkWriteDescriptorSet,10> writes{};
    for(uint32_t i=0;i<writes.size();++i) {
        auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=sets_[slot];w.dstBinding=i;w.descriptorCount=1;
        if(i==1 || i==2){w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&images[i-1];}
        else {w.descriptorType=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&buffers[i];}
    }
    vkUpdateDescriptorSets(ctx_.getDevice(),writes.size(),writes.data(),0,nullptr);
}
VkDescriptorBufferInfo SurfaceRt::output() const { return {filtered_.buffer,0,outputBytes_}; }
void SurfaceRt::record(VkCommandBuffer cmd) {
    static const bool profile=core::envFlagEnabled("WOWEE_SURFACE_RT_PROFILE",false);
    const bool firstRun=profile && recordedFrame_!=preparedFrame_;
    if(firstRun)ctx_.gpuMark(cmd,"WMO before first surface RT");
    // Orders readers from the previous run/submission before shared scratch is
    // overwritten. The surface render pass publishes its MRT writes to compute.
    barrier(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_SHADER_SAMPLED_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const uint32_t initial[]={0,0,0,0,0,1,1,0,0,1,1,0};vkCmdUpdateBuffer(cmd,indirect_.buffer,0,sizeof(initial),initial);
    const auto slot=ctx_.getCurrentFrame();
    if(recordedFrame_!=preparedFrame_) {
        vkCmdFillBuffer(cmd,history_[slot].buffer,0,historyBytes_,0);
        if(!historyValid_)vkCmdFillBuffer(cmd,history_[(slot+MAX_FRAMES_IN_FLIGHT-1)%MAX_FRAMES_IN_FLIGHT].buffer,0,historyBytes_,0);
    }
    barrier(cmd,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkDescriptorSet sets[]={sets_[slot],scene_.descriptorSet()};
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,2,sets,0,nullptr);
    const uint32_t push[]={sampleCount_,maxDispatch_,filtersPerTile_};
    vkCmdPushConstants(cmd,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
    const auto ms=samples_==VK_SAMPLE_COUNT_1_BIT?0:1;
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,base_[ms]);
    vkCmdDispatch(cmd,(extent_.width+15)/16,(extent_.height+15)/16,1);
    if(firstRun)ctx_.gpuMark(cmd,"surface RT first base");
    barrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,extra_[ms]);
    vkCmdDispatchIndirect(cmd,indirect_.buffer,16);
    if(firstRun)ctx_.gpuMark(cmd,"surface RT first extra");
    barrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,filter_[ms]);
    vkCmdDispatchIndirect(cmd,indirect_.buffer,32);
    if(firstRun)ctx_.gpuMark(cmd,"surface RT first filter");
    barrier(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    previousCamera_=glm::vec3(frameData_.cameraPos);previousViewProj_=glm::inverse(frameData_.invViewProj);
    previousMode_=int(frameData_.params.x);previousDistance_=frameData_.misc.w;recordedFrame_=preparedFrame_;
}
}
