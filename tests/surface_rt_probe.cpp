#include "surface_rt_probe.hpp"
#include "rendering/surface_rt.hpp"
#include "rendering/wmo_surface_pass.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>

using namespace wowee::rendering;
namespace {
void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
RtScene::MeshSource blocker() {
    RtScene::MeshSource mesh;
    // A square opaque caster at x=15, in front of the distant receiver.
    mesh.positions={{15,-2,-2},{15,2,-2},{15,2,2},{15,-2,2}};
    mesh.indices={0,1,2,0,2,3};mesh.surfaces.push_back(packRtSurface(glm::vec3(.6f),1));
    return mesh;
}
}
int checkSurfaceRt(VkContext& ctx) {
    constexpr uint32_t size=64;
    int failures=0;
    auto expect=[&](const char* label,bool ok){std::printf("%s: %s\n",ok?"ok":"FAIL",label);if(!ok)++failures;};
    for(auto samples:{VK_SAMPLE_COUNT_1_BIT,VK_SAMPLE_COUNT_4_BIT}) {
        RtScene scene;require(scene.initialize(&ctx),"probe scene initialization failed");
        scene.setActive(true);
        const glm::vec3 camera(5800,600,650);
        const auto caster=scene.addInstance(scene.addMesh(blocker()),glm::translate(glm::mat4(1),camera));
        const auto device=ctx.getDevice();const auto alloc=ctx.getAllocator();
        std::array<AllocatedImage,2> images;
        for(auto& image:images)image=createImage(device,alloc,size,size,VK_FORMAT_R32G32B32A32_UINT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,samples);
        VkAttachmentDescription descriptions[2]{};VkAttachmentReference colors[2]{};
        for(uint32_t i=0;i<2;++i) {
            auto& a=descriptions[i];a.format=VK_FORMAT_R32G32B32A32_UINT;a.samples=samples;
            a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            colors[i]={i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=2;sub.pColorAttachments=colors;
        VkSubpassDependency deps[2]{sceneAttachmentDependency(),{}};
        deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;deps[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        passInfo.attachmentCount=2;passInfo.pAttachments=descriptions;passInfo.subpassCount=1;passInfo.pSubpasses=&sub;
        passInfo.dependencyCount=2;passInfo.pDependencies=deps;
        VkRenderPass pass;require(vkCreateRenderPass(device,&passInfo,nullptr,&pass)==VK_SUCCESS,"probe render pass failed");
        VkImageView views[]={images[0].imageView,images[1].imageView};
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass=pass;fbInfo.attachmentCount=2;fbInfo.pAttachments=views;fbInfo.width=fbInfo.height=size;fbInfo.layers=1;
        VkFramebuffer framebuffer;require(vkCreateFramebuffer(device,&fbInfo,nullptr,&framebuffer)==VK_SUCCESS,"probe framebuffer failed");
        const auto layout=createPipelineLayout(device,{},{{VK_SHADER_STAGE_FRAGMENT_BIT,0,32}});
        wowee::rendering::VkShaderModule vertex,fragment;
        require(vertex.loadFromFile(device,"assets/shaders/wmo_lighting.vert.spv") &&
                fragment.loadFromFile(device,"assets/shaders/surface_rt_receivers.frag.spv"),"probe shaders unavailable");
        auto pipeline=PipelineBuilder().setShaders(vertex.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),fragment.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setRasterization(VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE).setNoDepthTest()
            .setColorBlendAttachments(std::vector<VkPipelineColorBlendAttachmentState>(2,PipelineBuilder::blendDisabled()))
            .setMultisample(samples).setLayout(layout).setRenderPass(pass).setDynamicStates(viewportAndScissorDynamic()).build(device,ctx.getPipelineCache());
        vertex.destroy();fragment.destroy();require(pipeline!=VK_NULL_HANDLE,"probe pipeline failed");
        const VkDeviceSize bytes=size*size*uint32_t(samples)*8;
        auto readback=createBuffer(alloc,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VMA_MEMORY_USAGE_CPU_ONLY);
        auto materials=createBuffer(alloc,2*sizeof(WmoSurfacePass::Material),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
        auto* data=static_cast<WmoSurfacePass::Material*>(materials.info.pMappedData);
        data[0]=WmoSurfacePass::Material{};data[1]=WmoSurfacePass::Material{};
        data[0].identity={1,0,1,0};data[1].identity={2,0,1,0};
        data[0].history.x=data[1].history.x=1;
        RtLighting::FrameInputs input{};
        input.cameraPos=camera;input.view=glm::lookAt(camera,camera+glm::vec3(1,0,0),glm::vec3(0,0,1));
        input.projection=glm::ortho(-8.f,8.f,-8.f,8.f,.5f,100.f);input.projection[1][1]*=-1;
        input.sunDir={-1,0,0};input.sunColor={1,1,1};input.skyColor={.2f,.2f,.2f};
        auto depth=[&](float x){auto p=input.projection*glm::mat4(glm::mat3(input.view))*glm::vec4(x,0,0,1);return p.z/p.w;};
        struct Push {glm::vec4 depths;glm::uvec4 config;} push{{depth(10),depth(20),0,0},{uint32_t(samples),0,0,0}};
        {
            SurfaceRt lighting(ctx,scene);
            auto frame=[&](int mode) {
                scene.applyChanges(scene.extractChanges());
                uint32_t presentImage=0;auto cmd=ctx.beginFrame(presentImage);require(cmd!=VK_NULL_HANDLE,"probe acquire failed");
                scene.update(cmd);
                require(scene.hasContent(),"probe tracing scene is empty");
                vmaFlushAllocation(alloc,materials.allocation,0,VK_WHOLE_SIZE);
                lighting.prepare({size,size},samples,views[0],views[1],{materials.buffer,0,2*sizeof(WmoSurfacePass::Material)},mode,input);
                VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea.extent={size,size};
                vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
                VkViewport viewport{0,0,float(size),float(size),0,1};VkRect2D scissor{{0,0},{size,size}};
                vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
                vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
                lighting.record(cmd);
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;barrier.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                barrier.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;barrier.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&barrier;cmdPipelineBarrier2(cmd,dep);
                VkBufferCopy copy{0,0,bytes};vkCmdCopyBuffer(cmd,lighting.output().buffer,readback.buffer,1,&copy);
                transitionImageLayout(cmd,ctx.getSwapchainImages()[presentImage],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                ctx.endFrame(cmd,presentImage);ctx.waitForSubmittedFrames("surface RT behavioral probe");
                vmaInvalidateAllocation(alloc,readback.allocation,0,VK_WHOLE_SIZE);
            };
            auto value=[&](uint32_t x,uint32_t y,uint32_t s){return static_cast<const glm::uvec2*>(readback.info.pMappedData)[(y*size+x)*uint32_t(samples)+s];};
            auto sun=[&](uint32_t x,uint32_t y,uint32_t s){return glm::unpackUnorm4x8(value(x,y,s).x).x;};
            frame(1);
            expect("base receiver in front of caster is lit in its first frame",sun(32,32,0)>.95f);
            expect("receiver missed by half-resolution base gets its own current shadow",sun(33,33,samples==1?0:1)<.05f);
            expect("unoccluded distant receiver stays lit",sun(5,5,samples==1?0:1)>.95f);
            if(samples!=1) {
                expect("same pixel/depth with a different sample normal is not aliased",sun(5,5,2)<.05f);
                expect("uncovered MSAA sample stays neutral",sun(32,32,3)>.95f);
            }
            // Make the back plane the base receiver so this exercises temporal
            // history, not just the missing-receiver queue (which has no history).
            push.config.y=1;
            for(int i=0;i<3;++i)frame(1);
            expect("base receiver records the caster shadow",sun(32,32,0)<.05f);
            scene.setInstanceTransform(caster,glm::translate(glm::mat4(1),camera+glm::vec3(0,40,0)));
            frame(1);
            expect("unchanged receiver accumulates its history with the new estimate",
                sun(32,32,0)>.1f && sun(32,32,0)<.5f);
            ++data[1].identity.x;
            frame(1);
            expect("replacement receiver rejects old lifetime's shadow history",sun(32,32,0)>.95f);
            frame(3);
            bool finite=true;
            for(uint32_t y=0;y<size;++y)for(uint32_t x=0;x<size;++x)for(uint32_t s=0;s<uint32_t(samples);++s) {
                auto packed=value(x,y,s);auto yz=glm::unpackHalf2x16(packed.y);
                finite=finite && std::isfinite(glm::unpackHalf1x16(uint16_t(packed.x>>16))) && std::isfinite(yz.x) && std::isfinite(yz.y);
            }
            expect("mode change produces finite current GI for every sample",finite);
        }
        ctx.flushDeferredCleanup();
        vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);
        vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,pass,nullptr);
        for(auto& image:images)destroyImage(device,alloc,image);
        destroyBuffer(alloc,materials);destroyBuffer(alloc,readback);
        scene.shutdown();ctx.flushDeferredCleanup();
    }
    return failures;
}
