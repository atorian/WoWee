#include "rendering/wmo_surface_pass.hpp"
#include "rendering/surface_rt.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include <cstring>
#include <stdexcept>

namespace wowee::rendering {
namespace {
constexpr std::array<VkFormat,3> formats{VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_UINT};
void requireSurface(bool ok, const char* what) {
    if (ok) return;
    LOG_ERROR("WMO surface pass: ", what, " - frame cannot be rendered");
    throw std::runtime_error(what);
}
void viewport(VkCommandBuffer cmd,VkExtent2D extent) {
    VkViewport v{0,0,float(extent.width),float(extent.height),0,1};
    VkRect2D sc{{0,0},extent};
    vkCmdSetViewport(cmd,0,1,&v); vkCmdSetScissor(cmd,0,1,&sc);
}
}
WmoSurfacePass::WmoSurfacePass(VkContext& ctx,VkDescriptorSetLayout frameLayout)
    :ctx_(ctx),frameLayout_(frameLayout) {}
WmoSurfacePass::~WmoSurfacePass() {
    retire();
    DeferredResources release(ctx_);
    for (auto& commands : commands_) release.release(commands.pool, vkDestroyCommandPool);
}
void WmoSurfacePass::retire() {
    DeferredResources release(ctx_);
    release.release(framebuffer_, vkDestroyFramebuffer); release.destroy(pipeline_); release.destroy(layout_);
    release.destroy(pool_); release.destroy(setLayout_); release.release(pass_, vkDestroyRenderPass);
    for(auto& image:images_) release.image(image);
    for(auto& buffer:buffers_) release.buffer(buffer);
    capacities_.fill(0); sets_.fill(VK_NULL_HANDLE); depth_=VK_NULL_HANDLE;
}
void WmoSurfacePass::create(VkExtent2D extent,VkImageView depth) {
    retire(); extent_=extent; depth_=depth; samples_=ctx_.getMsaaSamples();
    depthFormat_=ctx_.getDepthFormat(); colorFormat_=ctx_.getSwapchainFormat();
    const auto device=ctx_.getDevice();
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(ctx_.getPhysicalDevice(),&props);
    requireSurface(props.limits.maxColorAttachments>=3,"three color attachments unsupported");
    requireSurface(samples_==VK_SAMPLE_COUNT_1_BIT || ctx_.isSampleShadingSupported(),"per-sample lighting unsupported");
    for(size_t i=0;i<formats.size();++i) {
        VkImageFormatProperties capabilities{};
        const auto usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        requireSurface(vkGetPhysicalDeviceImageFormatProperties(ctx_.getPhysicalDevice(),formats[i],
            VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&capabilities)==VK_SUCCESS &&
            (capabilities.sampleCounts&samples_) && extent.width<=capabilities.maxExtent.width &&
            extent.height<=capabilities.maxExtent.height,"surface format/extent/sample count unsupported");
        images_[i]=createImage(device,ctx_.getAllocator(),extent.width,extent.height,formats[i],usage,samples_);
        requireSurface(images_[i].imageView!=VK_NULL_HANDLE,"surface image allocation failed");
    }
    VkAttachmentDescription attachments[4]{};
    VkAttachmentReference colors[3]{};
    for(uint32_t i=0;i<3;++i) {
        auto& a=attachments[i]; a.format=formats[i];a.samples=samples_;
        a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colors[i]={i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    auto& a=attachments[3];a.format=depthFormat_;a.samples=samples_;
    a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{3,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount=3;sub.pColorAttachments=colors;sub.pDepthStencilAttachment=&depthRef;
    // Includes previous submissions' readers of the same transient images.
    VkSubpassDependency deps[2]{sceneAttachmentDependency(),{}};
    deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount=4;info.pAttachments=attachments;info.subpassCount=1;info.pSubpasses=&sub;
    info.dependencyCount=2;info.pDependencies=deps;
    requireSurface(vkCreateRenderPass(device,&info,nullptr,&pass_)==VK_SUCCESS,"geometry pass creation failed");
    VkImageView views[]{images_[0].imageView,images_[1].imageView,images_[2].imageView,depth};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass=pass_;fb.attachmentCount=4;fb.pAttachments=views;fb.width=extent.width;fb.height=extent.height;fb.layers=1;
    requireSurface(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer_)==VK_SUCCESS,"surface framebuffer creation failed");
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(uint32_t i=0;i<5;++i) bindings.push_back({i,i>=3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
    setLayout_=createDescriptorSetLayout(device,bindings);
    requireSurface(setLayout_!=VK_NULL_HANDLE,"surface descriptor layout creation failed");
    VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3*kSlots},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2*kSlots}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets=kSlots;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
    requireSurface(vkCreateDescriptorPool(device,&pi,nullptr,&pool_)==VK_SUCCESS,"surface descriptor pool creation failed");
    for(auto& set:sets_) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=pool_;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout_;
        requireSurface(vkAllocateDescriptorSets(device,&ai,&set)==VK_SUCCESS,"surface descriptor allocation failed");
    }
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,80};
    layout_=createPipelineLayout(device,{frameLayout_,setLayout_},{push});
    VkShaderModule vert,frag;
    requireSurface(vert.loadFromFile(device,"assets/shaders/wmo_lighting.vert.spv") &&
        frag.loadFromFile(device,samples_==VK_SAMPLE_COUNT_1_BIT?"assets/shaders/wmo_lighting.frag.spv":"assets/shaders/wmo_lighting_ms.frag.spv"),"lighting shaders unavailable");
    pipeline_=PipelineBuilder().setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setRasterization(VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE).setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendDisabled()).setMultisample(samples_)
        .setLayout(layout_).setRenderPass(ctx_.getImGuiRenderPass()).setDynamicStates(viewportAndScissorDynamic())
        .build(device,ctx_.getPipelineCache());
    vert.destroy();frag.destroy();requireSurface(pipeline_!=VK_NULL_HANDLE,"lighting pipeline creation failed");
    LOG_INFO("WMO evaluated surfaces: ",extent.width,"x",extent.height," samples=",int(samples_),
        " transient color bytes=",uint64_t(extent.width)*extent.height*40*uint32_t(samples_));
}
void WmoSurfacePass::prepare(VkExtent2D extent,VkImageView depth,TargetVersion version,size_t materialCapacity,
        RtScene* rtScene,int rtMode,const RtLighting::FrameInputs& rtInputs) {
    requireSurface(depth!=VK_NULL_HANDLE,"scene depth unavailable");
    if(!pass_ || version!=targetVersion_ || depth!=depth_ || extent.width!=extent_.width || extent.height!=extent_.height ||
       samples_!=ctx_.getMsaaSamples() || colorFormat_!=ctx_.getSwapchainFormat() || depthFormat_!=ctx_.getDepthFormat()) create(extent,depth);
    targetVersion_=version;
    const uint32_t slot=ctx_.getCurrentFrame();
    auto& commands = commands_[slot];
    if (!commands.pool) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = ctx_.getGraphicsQueueFamily();
        requireSurface(vkCreateCommandPool(ctx_.getDevice(), &info, nullptr, &commands.pool) == VK_SUCCESS,
                       "worker command pool creation failed");
    }
    requireSurface(vkResetCommandPool(ctx_.getDevice(), commands.pool, 0) == VK_SUCCESS,
                   "worker command pool reset failed");
    commands.runs.clear();
    materialLimit_=materialCapacity; materialsWritten_=0;
    const size_t bytes=std::max(size_t(1),materialCapacity)*sizeof(Material);
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(ctx_.getPhysicalDevice(),&props);
    requireSurface(bytes<=props.limits.maxStorageBufferRange,"surface material table exceeds device limit");
    if(capacities_[slot]<bytes) {
        DeferredResources release(ctx_);release.buffer(buffers_[slot]);
        buffers_[slot]=createBuffer(ctx_.getAllocator(),bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
        requireSurface(buffers_[slot].info.pMappedData!=nullptr,"surface material upload allocation failed");
        capacities_[slot]=bytes;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.maxLod=0;
    si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorImageInfo imageInfos[3]{};VkWriteDescriptorSet writes[5]{};
    for(uint32_t i=0;i<5;++i) {
        auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=sets_[slot];w.dstBinding=i;w.descriptorCount=1;
        w.descriptorType=i>=3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if(i<3) {imageInfos[i]={ctx_.getOrCreateSampler(si),images_[i].imageView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};w.pImageInfo=&imageInfos[i];}
    }
    VkDescriptorBufferInfo buffer{buffers_[slot].buffer,0,bytes};writes[3].pBufferInfo=&buffer;
    rtMode_=rtMode;rtDistance_=rtInputs.distance;
    VkDescriptorBufferInfo lighting=buffer; // Bound but unread when RT is off.
    if(rtMode_!=0) {
        requireSurface(rtScene!=nullptr,"current RT scene unavailable");
        if(!rt_)rt_=std::make_unique<SurfaceRt>(ctx_,*rtScene);
        rt_->prepare(extent,samples_,images_[1].imageView,images_[2].imageView,buffer,rtMode_,rtInputs);
        lighting=rt_->output();
    }
    writes[4].pBufferInfo=&lighting;
    vkUpdateDescriptorSets(ctx_.getDevice(),5,writes,0,nullptr);
}
bool WmoSurfacePass::rtHistoryValid() const { return rtMode_!=0 && rt_ && rt_->hasHistory(); }
glm::vec3 WmoSurfacePass::previousRtCamera() const { return rt_?rt_->previousCamera():glm::vec3(0); }
uint32_t WmoSurfacePass::appendMaterial(const Material& material) {
    requireSurface(materialsWritten_<materialLimit_,"snapshot material capacity exhausted");
    const uint32_t index=static_cast<uint32_t>(materialsWritten_++);
    auto* output=static_cast<Material*>(buffers_[ctx_.getCurrentFrame()].info.pMappedData);
    std::memcpy(output+index,&material,sizeof(material));
    return index;
}
void WmoSurfacePass::finishMaterials() {
    // Slot fence was acquired before recording. Host writes, including those
    // made after recording a draw, are flushed before this command is submitted.
    requireSurface(vmaFlushAllocation(ctx_.getAllocator(),buffers_[ctx_.getCurrentFrame()].allocation,
        0,VK_WHOLE_SIZE)==VK_SUCCESS,"surface material upload flush failed");
}
VkCommandBuffer WmoSurfacePass::beginRun(bool geometry) {
    auto& commands = commands_[ctx_.getCurrentFrame()];
    const size_t index = commands.runs.size();
    if (index == commands.buffers.size()) {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = commands.pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        info.commandBufferCount = 1;
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        requireSurface(vkAllocateCommandBuffers(ctx_.getDevice(), &info, &buffer) == VK_SUCCESS,
                       "worker command buffer allocation failed");
        commands.buffers.push_back(buffer);
    }
    const auto cmd = commands.buffers[index];
    VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    inheritance.renderPass = geometry ? pass_ : ctx_.getSceneContinueRenderPass();
    // No framebuffer binding: the run remains valid for the current scene target.
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin.pInheritanceInfo = &inheritance;
    requireSurface(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS, "worker command recording failed");
    viewport(cmd, extent_);
    commands.runs.push_back({cmd, geometry});
    return cmd;
}
void WmoSurfacePass::endRun() {
    requireSurface(vkEndCommandBuffer(commands_[ctx_.getCurrentFrame()].runs.back().command) == VK_SUCCESS,
                   "worker command recording completion failed");
}
void WmoSurfacePass::execute(VkCommandBuffer cmd, VkDescriptorSet frameSet, VkFramebuffer scene,
        const glm::mat4& inverseViewProjection, VkSubpassContents resumeContents) {
    auto contents = resumeContents;
    for (const auto& run : commands_[ctx_.getCurrentFrame()].runs) {
        if (run.geometry) {
            beginGeometry(cmd);
            vkCmdExecuteCommands(cmd, 1, &run.command);
            light(cmd, frameSet, scene, inverseViewProjection);
            contents = VK_SUBPASS_CONTENTS_INLINE;
        } else {
            if (contents != VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) {
                vkCmdEndRenderPass(cmd);
                resumeScene(cmd, scene, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
            }
            vkCmdExecuteCommands(cmd, 1, &run.command);
            contents = VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
        }
    }
    if (contents != resumeContents) {
        vkCmdEndRenderPass(cmd);
        resumeScene(cmd, scene, resumeContents);
    }
}
void WmoSurfacePass::beginGeometry(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
    VkClearValue clear[3]{};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=pass_;begin.framebuffer=framebuffer_;begin.renderArea.extent=extent_;
    begin.clearValueCount=3;begin.pClearValues=clear;
    vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
}
void WmoSurfacePass::resumeScene(VkCommandBuffer cmd,VkFramebuffer scene,VkSubpassContents contents) {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=ctx_.getSceneContinueRenderPass();begin.framebuffer=scene;begin.renderArea.extent=extent_;
    vkCmdBeginRenderPass(cmd,&begin,contents);
    if(contents==VK_SUBPASS_CONTENTS_INLINE) viewport(cmd,extent_);
}
void WmoSurfacePass::light(VkCommandBuffer cmd,VkDescriptorSet frameSet,VkFramebuffer scene,const glm::mat4& inverseViewProjection) {
    vkCmdEndRenderPass(cmd);
    if(rtMode_!=0)rt_->record(cmd);
    resumeScene(cmd,scene,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    VkDescriptorSet sets[]{frameSet,sets_[ctx_.getCurrentFrame()]};
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,2,sets,0,nullptr);
    struct Push {glm::mat4 inverse;glm::vec4 extent;} push{inverseViewProjection,{extent_.width,extent_.height,rtMode_,rtDistance_}};
    vkCmdPushConstants(cmd,layout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    vkCmdDraw(cmd,3,1,0,0);
}
}
