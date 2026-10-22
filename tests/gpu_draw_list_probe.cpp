#include "gpu_draw_list_probe.hpp"
#include "rendering/gpu_draw_list.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/camera.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace wowee::rendering;
int checkGpuDrawList(VkContext& ctx) {
    int failures=0;
    auto expect=[&](const char* text,bool ok) {
        std::printf("%s: %s\n",ok?"ok":"FAIL",text);if(!ok)++failures;
    };
    const auto alloc=ctx.getAllocator();
    constexpr uint32_t maxCandidates=143;
    // Reading the public draw/visible buffers here tests the GPU consumer
    // contract. Production submission never reads these buffers on the host.
    auto readback=createBuffer(alloc,4096,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VMA_MEMORY_USAGE_CPU_ONLY);
    {
        GpuDrawList list(ctx);
        Camera camera;camera.setPosition({0,0,0});camera.setRotation(0,0);
        std::vector<uint32_t> expected;
        for(uint32_t turn=0;turn<5;++turn) {
            uint32_t image=0;auto cmd=ctx.beginFrame(image);
            if(!cmd)throw std::runtime_error("GPU draw probe cannot acquire frame");
            const bool empty=turn==3,small=turn==4,force=turn==2;
            const uint32_t count=empty?0:small?5:133;
            list.prepare(ctx.getCurrentFrame(),empty?0:maxCandidates,empty?0:4);
            camera.setRotation(turn==1?180.f:0.f,0);
            expected.clear();
            for(uint32_t i=0;i<count;++i) {
                const bool front=small || i%3==0;
                list.appendSphere({front?10.f:-10.f,0,0,.1f});
                if(force || (turn==1?!front:front))expected.push_back(i);
            }
            std::array<VkDeviceSize,4> offsets{};
            if(!empty) {
                const auto group=list.addGroup(0,count);
                offsets[0]=list.addDraw(group,6,5,-2);
                offsets[1]=list.addDraw(group,18,23,4);
                for(uint32_t i=0;i<7;++i)list.appendSphere({-10,0,0,.1f});
                const auto back=list.addGroup(count,7);
                offsets[2]=list.addDraw(back,12,7);
                for(uint32_t i=0;i<3;++i)list.appendSphere({10,0,0,.1f});
                const auto front=list.addGroup(count+7,3);
                offsets[3]=list.addDraw(front,9,11);
            }
            list.record(cmd,camera,force);
            if(!empty) {
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
                barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                std::array<VkBufferCopy,4> commands{};
                for(uint32_t i=0;i<4;++i)commands[i]={offsets[i],i*sizeof(VkDrawIndexedIndirectCommand),sizeof(VkDrawIndexedIndirectCommand)};
                vkCmdCopyBuffer(cmd,list.drawBuffer(),readback.buffer,4,commands.data());
                VkBufferCopy visible{0,128,VkDeviceSize(count+10)*sizeof(uint32_t)};
                vkCmdCopyBuffer(cmd,list.visibleBuffer(),readback.buffer,1,&visible);
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            }
            transitionImageLayout(cmd,ctx.getSwapchainImages()[image],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            ctx.endFrame(cmd,image);
            ctx.waitForSubmittedFrames("GPU draw behavior probe");
            if(empty) {
                expect("empty captured view has no draw commands",!list.hasDraws());
                continue;
            }
            vmaInvalidateAllocation(alloc,readback.allocation,0,VK_WHOLE_SIZE);
            const auto* commands=static_cast<const VkDrawIndexedIndirectCommand*>(readback.info.pMappedData);
            const auto* visible=reinterpret_cast<const uint32_t*>(static_cast<const std::byte*>(readback.info.pMappedData)+128);
            expect("both material batches draw the current visible instance count",
                commands[0].instanceCount==expected.size() && commands[1].instanceCount==expected.size());
            expect("compaction preserves source order across workgroup chunks and partial tails",
                std::equal(expected.begin(),expected.end(),visible));
            expect("indexed geometry ranges survive GPU count generation",
                commands[0].indexCount==6 && commands[0].firstIndex==5 && commands[0].vertexOffset==-2 &&
                commands[1].indexCount==18 && commands[1].firstIndex==23 && commands[1].vertexOffset==4);
            expect("fully hidden and visible groups use this frame's camera and force mode",
                commands[2].instanceCount==((turn==1 || force)?7u:0u) &&
                commands[3].instanceCount==(turn==1?0u:3u));
        }
    }
    destroyBuffer(alloc,readback);ctx.flushDeferredCleanup();return failures;
}
