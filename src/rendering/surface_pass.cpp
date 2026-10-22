#include "rendering/surface_pass.hpp"
#include "rendering/surface_rt.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include "core/env_flag.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wowee::rendering {
namespace {
constexpr std::array<VkFormat,3> formats{VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_UINT};
void requireSurface(bool ok, const char* what) {
    if (ok) return;
    LOG_ERROR("Evaluated surface pass: ", what, " - frame cannot be rendered");
    throw std::runtime_error(what);
}
void viewport(VkCommandBuffer cmd,VkExtent2D extent) {
    VkViewport v{0,0,float(extent.width),float(extent.height),0,1};
    VkRect2D sc{{0,0},extent};
    vkCmdSetViewport(cmd,0,1,&v); vkCmdSetScissor(cmd,0,1,&sc);
}
}
SurfacePass::SurfacePass(VkContext& ctx,VkDescriptorSetLayout frameLayout)
    :ctx_(ctx),frameLayout_(frameLayout) {}
SurfacePass::~SurfacePass() {
    retire();
    DeferredResources release(ctx_);
    for (auto& slot : commands_) for(auto& commands:slot) release.release(commands.pool, vkDestroyCommandPool);
}
void SurfacePass::retire() {
    DeferredResources release(ctx_);
    release.release(framebuffer_, vkDestroyFramebuffer); release.destroy(pipeline_); release.destroy(layout_);
    release.destroy(pool_); release.destroy(setLayout_); release.release(pass_, vkDestroyRenderPass);
    for(auto& image:images_) release.image(image);
    for(auto& buffer:buffers_) release.buffer(buffer);
    capacities_.fill(0); sets_.fill(VK_NULL_HANDLE); depth_=VK_NULL_HANDLE;
    for(auto& table:materialTables_)for(auto& versions:table.uploaded)versions.clear();
}
void SurfacePass::create(VkExtent2D extent,VkImageView depth) {
    // These transient targets are not referenced by producer snapshots. This
    // runs before this frame records any surface command. Complete only older
    // submitted frames and free their targets before allocating replacements,
    // avoiding two full-resolution MSAA G-buffers resident during a resize.
    if(framebuffer_) {
        requireSurface(ctx_.waitForSubmittedFrames("surface target resize")==VK_SUCCESS,"target retirement wait failed");
        vkDestroyFramebuffer(ctx_.getDevice(),framebuffer_,nullptr);framebuffer_=VK_NULL_HANDLE;
        for(auto& image:images_)destroyImage(ctx_.getDevice(),ctx_.getAllocator(),image);
        for(auto& buffer:buffers_)destroyBuffer(ctx_.getAllocator(),buffer);
    }
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
    {
    VkShaderModule vert,frag;
    requireSurface(vert.loadFromFile(device,"assets/shaders/wmo_lighting.vert.spv") &&
        frag.loadFromFile(device, (std::string("assets/shaders/")+"surface_lighting"+(samples_==VK_SAMPLE_COUNT_1_BIT?"":"_ms")+".frag.spv").c_str()),"lighting shaders unavailable");
    pipeline_=PipelineBuilder().setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setRasterization(VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE).setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendDisabled()).setMultisample(samples_)
        .setLayout(layout_).setRenderPass(ctx_.getImGuiRenderPass()).setDynamicStates(viewportAndScissorDynamic())
        .build(device,ctx_.getPipelineCache());
    vert.destroy();frag.destroy();requireSurface(pipeline_!=VK_NULL_HANDLE,"lighting pipeline creation failed");
    }
    LOG_INFO("Shared evaluated surfaces: ",extent.width,"x",extent.height," samples=",int(samples_),
        " transient color bytes=",uint64_t(extent.width)*extent.height*40*uint32_t(samples_));
}
void SurfacePass::prepare(VkExtent2D extent,VkImageView depth,TargetVersion version,std::array<size_t,kFamilies> materialCapacities,
        RtScene* rtScene,int rtMode,const RtFrameInputs& rtInputs) {
    requireSurface(depth!=VK_NULL_HANDLE,"scene depth unavailable");
    if(!pass_ || version!=targetVersion_ || depth!=depth_ || extent.width!=extent_.width || extent.height!=extent_.height ||
       samples_!=ctx_.getMsaaSamples() || colorFormat_!=ctx_.getSwapchainFormat() || depthFormat_!=ctx_.getDepthFormat()) create(extent,depth);
    targetVersion_=version;
    const uint32_t slot=ctx_.getCurrentFrame();
    for(auto& commands:commands_[slot]) {
    if (!commands.pool) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = ctx_.getGraphicsQueueFamily();
        requireSurface(vkCreateCommandPool(ctx_.getDevice(), &info, nullptr, &commands.pool) == VK_SUCCESS,
                       "worker command pool creation failed");
    }
    requireSurface(vkResetCommandPool(ctx_.getDevice(), commands.pool, 0) == VK_SUCCESS,
                   "worker command pool reset failed");
    commands.runs.clear();
    }
    size_t materialCapacity=0;
    for(size_t family=0;family<kFamilies;++family) {
        auto& table=materialTables_[family];
        // Reclaim a departed view's table, or compact after a large population
        // drop. Only metadata is moved; already submitted slots own their bytes.
        if(materialCapacities[family]==0) {
            table=MaterialTable{};
        } else if(table.records.size()>256 && table.indices.size()<table.records.size()/3) {
            std::vector<MaterialRecord> compact;
            compact.reserve(table.indices.size());table.previousUsed.clear();
            for(auto& [key,index]:table.indices) {
                compact.push_back(table.records[index]);index=uint32_t(compact.size()-1);
                table.previousUsed.push_back(index);
            }
            table.records=std::move(compact);table.free.clear();
            for(auto& versions:table.uploaded)versions.clear();
        }
        table.currentUsed.clear();table.uploadCount=0;
        table.uploadFirst=std::numeric_limits<size_t>::max();table.uploadEnd=0;
        table.begin=materialCapacity;
        // Reserve the worst case before workers start: all current identities
        // may be new while the preceding frame's live keys still occupy rows.
        const size_t newRows=materialCapacities[family]>table.free.size()?materialCapacities[family]-table.free.size():0;
        table.capacity=table.records.size()+newRows;
        table.records.reserve(table.capacity);
        table.currentUsed.reserve(materialCapacities[family]);
        table.indices.reserve(table.indices.size()+newRows);
        if(uploadedMaterialBegins_[slot][family]!=table.begin) table.uploaded[slot].clear();
        uploadedMaterialBegins_[slot][family]=table.begin;
        table.uploaded[slot].resize(table.capacity,0);
        materialsWritten_[family]=0;materialLimit_[family]=materialCapacities[family];
        materialCapacity+=table.capacity;
    }
    const size_t bytes=std::max(size_t(1),materialCapacity)*sizeof(Material);
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(ctx_.getPhysicalDevice(),&props);
    requireSurface(bytes<=props.limits.maxStorageBufferRange,"surface material table exceeds device limit");
    if(capacities_[slot]<bytes || (capacities_[slot]>65536 && bytes<capacities_[slot]/3)) {
        DeferredResources release(ctx_);release.buffer(buffers_[slot]);
        buffers_[slot]=createBuffer(ctx_.getAllocator(),bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
        requireSurface(buffers_[slot].info.pMappedData!=nullptr,"surface material upload allocation failed");
        capacities_[slot]=bytes;
        for(auto& table:materialTables_)std::fill(table.uploaded[slot].begin(),table.uploaded[slot].end(),0);
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
bool SurfacePass::rtHistoryValid() const { return rtMode_!=0 && rt_ && rt_->hasHistory(); }
glm::vec3 SurfacePass::previousRtCamera() const { return rt_?rt_->previousCamera():glm::vec3(0); }
uint32_t SurfacePass::appendMaterial(const Material& material) { return appendMaterialBytes(Family::Wmo,&material); }
uint32_t SurfacePass::appendMaterial(const M2Material& material) {
    static_assert(sizeof(Material)==sizeof(M2Material));
    return appendMaterialBytes(Family::M2,&material);
}
uint32_t SurfacePass::appendMaterial(Family family, const M2Material& material) {
    return appendMaterialBytes(family,&material);
}
SurfacePass::MaterialKey SurfacePass::materialKey(const Material& material) {
    return {uint64_t(material.identity.x)|(uint64_t(material.identity.y)<<32),
            uint64_t(material.identity.z)|(uint64_t(material.identity.w)<<32)};
}
uint32_t SurfacePass::appendMaterialBytes(Family kind,const void* material) {
    const size_t family=size_t(kind);
    requireSurface(materialsWritten_[family]<materialLimit_[family],"snapshot material capacity exhausted");
    ++materialsWritten_[family];
    auto& table=materialTables_[family];
    Material value{};std::memcpy(&value,material,sizeof(value));value.history.y=uint32_t(family);
    const auto key=materialKey(value);
    requireSurface(key.placement!=0,"surface material lacks a lifetime identity");
    auto at=table.indices.find(key);
    uint32_t row;
    if(at==table.indices.end()) {
        if(table.free.empty()) {
            row=uint32_t(table.records.size());table.records.emplace_back();
        } else {
            row=table.free.back();table.free.pop_back();table.records[row]=MaterialRecord{};
        }
        table.indices.emplace(key,row);
    } else row=at->second;
    requireSurface(row<table.capacity,"persistent material reservation exhausted");
    auto& record=table.records[row];
    const uint64_t frame=ctx_.frameNumber();
    if(record.version==0 || std::memcmp(&record.value,&value,sizeof(value))!=0) {
        // An earlier draw in this same frame must never see a later draw's
        // value under the same key. A real material variant needs its own ID.
        requireSurface(record.seenFrame!=frame,"one surface identity has conflicting frame materials");
        record.value=value;record.version=++table.nextVersion;
    }
    if(record.seenFrame!=frame) {
        table.currentUsed.push_back(row);record.seenFrame=frame;
    }
    const auto slot=ctx_.getCurrentFrame();
    const uint32_t index=uint32_t(table.begin+row);
    if(table.uploaded[slot][row]!=record.version) {
        auto* output=static_cast<Material*>(buffers_[slot].info.pMappedData);
        std::memcpy(output+index,&record.value,sizeof(value));
        table.uploaded[slot][row]=record.version;
        table.uploadFirst=std::min(table.uploadFirst,size_t(index));
        table.uploadEnd=std::max(table.uploadEnd,size_t(index)+1);++table.uploadCount;
    }
    return index;
}
void SurfacePass::finishMaterials() {
    const auto slot=ctx_.getCurrentFrame();const auto frame=ctx_.frameNumber();
    size_t uploaded=0,live=0;
    for(auto& table:materialTables_) {
        // This is a list of previously referenced table rows, never a walk of
        // the scene. Other slots retain their old row values until fenced.
        for(const auto row:table.previousUsed) if(table.records[row].seenFrame!=frame) {
            table.indices.erase(materialKey(table.records[row].value));
            table.free.push_back(row);
        }
        table.previousUsed.swap(table.currentUsed);
        if(table.uploadCount)requireSurface(vmaFlushAllocation(ctx_.getAllocator(),buffers_[slot].allocation,
            table.uploadFirst*sizeof(Material),(table.uploadEnd-table.uploadFirst)*sizeof(Material))==VK_SUCCESS,
            "surface material upload flush failed");
        uploaded+=table.uploadCount;live+=table.indices.size();
    }
    if(core::envFlagEnabled("WOWEE_SURFACE_PLAN_CHECK",false))
        LOG_INFO("Surface material table: live=",live," uploaded bytes=",uploaded*sizeof(Material));
}
VkRect2D SurfacePass::projectBounds(const glm::mat4& clipFromLocal,glm::vec3 lo,glm::vec3 hi) const {
    static const bool full=core::envFlagEnabled("WOWEE_SURFACE_FULL_EXTENT",false);
    if(full)return fullRegion();
    glm::vec2 minimum(1.f),maximum(-1.f);
    for(unsigned i=0;i<8;++i) {
        const glm::vec4 clip=clipFromLocal*glm::vec4(i&1?hi.x:lo.x,i&2?hi.y:lo.y,i&4?hi.z:lo.z,1);
        if(!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.w) || clip.w<=0.f)
            return fullRegion();
        const glm::vec2 ndc=glm::vec2(clip)/clip.w;
        minimum=glm::min(minimum,ndc);maximum=glm::max(maximum,ndc);
    }
    // Outward rounding plus a pixel protects raster/sample rounding. Align to
    // RT tiles so each invocation still owns the same global 2x2 receivers.
    const auto axis=[](float lo,float hi,uint32_t extent) {
        const float a=std::clamp((lo*.5f+.5f)*extent-1.f,0.f,float(extent));
        const float b=std::clamp((hi*.5f+.5f)*extent+1.f,0.f,float(extent));
        const uint32_t first=uint32_t(std::floor(a))/16u*16u;
        const uint32_t last=std::min(extent,(uint32_t(std::ceil(b))+15u)/16u*16u);
        return std::pair{first,last};
    };
    const auto x=axis(minimum.x,maximum.x,extent_.width),y=axis(minimum.y,maximum.y,extent_.height);
    return {{int32_t(x.first),int32_t(y.first)},{x.second-x.first,y.second-y.first}};
}
VkRect2D SurfacePass::uniteRegions(VkRect2D a,VkRect2D b) {
    if(!a.extent.width || !a.extent.height)return b;
    if(!b.extent.width || !b.extent.height)return a;
    const auto right=std::max(a.offset.x+int32_t(a.extent.width),b.offset.x+int32_t(b.extent.width));
    const auto bottom=std::max(a.offset.y+int32_t(a.extent.height),b.offset.y+int32_t(b.extent.height));
    const VkOffset2D offset{std::min(a.offset.x,b.offset.x),std::min(a.offset.y,b.offset.y)};
    return {offset,{uint32_t(right-offset.x),uint32_t(bottom-offset.y)}};
}
void SurfacePass::includeRegion(Family family,VkRect2D region) {
    auto& target=commands_[ctx_.getCurrentFrame()][size_t(family)].runs.back().region;
    target=uniteRegions(target,region);
}
VkCommandBuffer SurfacePass::beginRun(Family family,bool geometry,bool bounded) {
    auto& commands = commands_[ctx_.getCurrentFrame()][size_t(family)];
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
    commands.runs.push_back({cmd, geometry,bounded?VkRect2D{}:fullRegion()});
    return cmd;
}
void SurfacePass::endRun(Family family) {
    requireSurface(vkEndCommandBuffer(commands_[ctx_.getCurrentFrame()][size_t(family)].runs.back().command) == VK_SUCCESS,
                   "worker command recording completion failed");
}
void SurfacePass::executePlan(std::span<const Operation> operations,VkCommandBuffer cmd,VkDescriptorSet frameSet,
        VkFramebuffer scene,const glm::mat4& inverseViewProjection,VkSubpassContents resumeContents) {
    // Flatten command metadata only. Geometry and materials were recorded once
    // by their family worker; this owner never revisits any scene instance.
    std::vector<Run> ordered;
    for(const auto& operation:operations) {
        if(operation.forward)ordered.push_back({operation.forward,false,fullRegion()});
        else if(operation.family!=Family::Count) {
            const auto& runs=commands_[ctx_.getCurrentFrame()][size_t(operation.family)].runs;
            ordered.insert(ordered.end(),runs.begin(),runs.end());
        }
    }
    uint32_t geometryRuns=0,lightingGroups=0;
    auto contents=resumeContents;
    for(size_t i=0;i<ordered.size();) {
        if(ordered[i].geometry) {
            size_t end=i;
            VkRect2D region{};
            while(end<ordered.size() && ordered[end].geometry) {
                region=uniteRegions(region,ordered[end].region);++end;
            }
            if(region.extent.width && region.extent.height) {
                geometryRuns+=uint32_t(end-i);++lightingGroups;
                beginGeometry(cmd,region);
                for(size_t j=i;j<end;++j) if(ordered[j].region.extent.width && ordered[j].region.extent.height)
                    vkCmdExecuteCommands(cmd,1,&ordered[j].command);
                light(Family::Wmo,cmd,frameSet,scene,inverseViewProjection,region,false);
                contents=VK_SUBPASS_CONTENTS_INLINE;
            }
            i=end;
        } else {
            if(contents!=VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) {
                vkCmdEndRenderPass(cmd);resumeScene(cmd,scene,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
            }
            vkCmdExecuteCommands(cmd,1,&ordered[i].command);++i;
            contents=VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
        }
    }
    if(core::envFlagEnabled("WOWEE_SURFACE_PLAN_CHECK",false))
        LOG_INFO("Surface plan: geometry runs=",geometryRuns," lighting groups=",lightingGroups);
    if(contents!=resumeContents) {
        vkCmdEndRenderPass(cmd);resumeScene(cmd,scene,resumeContents);
    } else if(contents==VK_SUBPASS_CONTENTS_INLINE)viewport(cmd,extent_);
}
void SurfacePass::beginGeometry(VkCommandBuffer cmd,VkRect2D region) {
    vkCmdEndRenderPass(cmd);
    VkClearValue clear[3]{};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=pass_;begin.framebuffer=framebuffer_;begin.renderArea=region;
    begin.clearValueCount=3;begin.pClearValues=clear;
    vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
}
void SurfacePass::resumeScene(VkCommandBuffer cmd,VkFramebuffer scene,VkSubpassContents contents) {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=ctx_.getSceneContinueRenderPass();begin.framebuffer=scene;begin.renderArea.extent=extent_;
    vkCmdBeginRenderPass(cmd,&begin,contents);
    if(contents==VK_SUBPASS_CONTENTS_INLINE) viewport(cmd,extent_);
}
void SurfacePass::light(Family family,VkCommandBuffer cmd,VkDescriptorSet frameSet,VkFramebuffer scene,const glm::mat4& inverseViewProjection,VkRect2D region,bool profile) {
    vkCmdEndRenderPass(cmd);
    if(rtMode_!=0)rt_->record(cmd,!profile?SurfaceRt::Profile::None:
        family==Family::M2?SurfaceRt::Profile::M2:SurfaceRt::Profile::Wmo,region);
    resumeScene(cmd,scene,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetScissor(cmd,0,1,&region);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    VkDescriptorSet sets[]{frameSet,sets_[ctx_.getCurrentFrame()]};
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,2,sets,0,nullptr);
    struct Push {glm::mat4 inverse;glm::vec4 extent;} push{inverseViewProjection,{extent_.width,extent_.height,rtMode_,rtDistance_}};
    vkCmdPushConstants(cmd,layout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    vkCmdDraw(cmd,3,1,0,0);
}
}
