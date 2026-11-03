// What a caller of VkContext may count on while an upload batch is open - and
// the frame thread keeps one open for whole phases of the frame.
//
// Work handed to immediateSubmit has run when it returns, so its caller may
// free what it read and use what it wrote: the login screen freed its staging
// buffer straight after, and portraits recorded only layout transitions, and
// in a batch both were left unsent or thrown away. A copy handed to
// submitUpload has run once the batch has. Both are read back from a buffer
// the host can see. Requires Vulkan; exits 77 without a device.
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

using namespace wowee;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

uint32_t readBack(rendering::VkContext& context, const rendering::AllocatedBuffer& buffer) {
    vmaInvalidateAllocation(context.getAllocator(), buffer.allocation, 0, sizeof(uint32_t));
    return *static_cast<const uint32_t*>(buffer.info.pMappedData);
}

void toHost(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    rendering::cmdPipelineBarrier2(cmd, dep);
}

void exercise(rendering::VkContext& context) {
    rendering::AllocatedBuffer target = rendering::createBuffer(
        context.getAllocator(), sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_TO_CPU);
    require(target.buffer != VK_NULL_HANDLE && target.info.pMappedData, "readback buffer unavailable");

    // Work with no staging buffer of its own, inside a batch, read the moment
    // the call returns.
    context.beginUploadBatch();
    context.immediateSubmit([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, target.buffer, 0, sizeof(uint32_t), 0xA5A5A5A5u);
        toHost(cmd);
    });
    const uint32_t filled = readBack(context, target);
    context.endUploadBatch();
    context.waitAllUploads();
    require(filled == 0xA5A5A5A5u, "immediateSubmit inside a batch had not run when it returned (read " +
                                       std::to_string(filled) + ")");

    // A copy left to the batch arrives once the batch has run.
    const uint32_t value = 0x5EED1234u;
    auto done = std::make_shared<std::atomic<int>>(0);
    context.beginUploadBatch();
    require(rendering::uploadIntoBuffer(context, &value, sizeof(value), target.buffer),
            "uploadIntoBuffer could not stage the copy");
    context.endUploadBatch(done);
    context.waitAllUploads();
    require(done->load() == 1, "the batch holding the copy did not complete");
    require(readBack(context, target) == value, "the batched copy did not reach its buffer");

    rendering::destroyBuffer(context.getAllocator(), target);
}
}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
    auto* window = SDL_CreateWindow("Upload batch test", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) { SDL_Quit(); return 77; }
    int result = 1;
    {
        rendering::VkContext context;
        try {
            require(context.initialize(window), "Vulkan initialization failed");
            exercise(context);
            result = 0;
        } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); }
        context.shutdown();
    }
    SDL_DestroyWindow(window); SDL_Quit();
    return result;
}
