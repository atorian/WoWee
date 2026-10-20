#include "rendering/vk_utils.hpp"

#include <cstdio>
#include "rendering/vk_context.hpp"
#include "core/logger.hpp"
#include <cstring>
#include <vector>

namespace wowee {
namespace rendering {

DeferredResources::DeferredResources(VkContext& ctx)
    : ctx_(ctx), device_(ctx.getDevice()), allocator_(ctx.getAllocator()) {}

DeferredResources::~DeferredResources() {
    if (cleanups_.empty()) return;
    ctx_.deferAfterAllFrameFences([cleanups = std::move(cleanups_)]() {
        for (auto& cleanup : cleanups) cleanup();
    });
}

void DeferredResources::buffer(VkBuffer& buffer, VmaAllocation& allocation) {
    if (!buffer) return;
    auto retired = std::exchange(buffer, VK_NULL_HANDLE);
    auto retiredAlloc = std::exchange(allocation, VK_NULL_HANDLE);
    auto allocator = allocator_;
    add([allocator, retired, retiredAlloc] { gpuDestroyBuffer(allocator, retired, retiredAlloc); });
}

void DeferredResources::buffer(AllocatedBuffer& value) {
    buffer(value.buffer, value.allocation);
    value = {};
}

void DeferredResources::image(VkImage& image, VmaAllocation& allocation) {
    if (!image) return;
    auto retired = std::exchange(image, VK_NULL_HANDLE);
    auto retiredAlloc = std::exchange(allocation, VK_NULL_HANDLE);
    auto allocator = allocator_;
    add([allocator, retired, retiredAlloc] { gpuDestroyImage(allocator, retired, retiredAlloc); });
}

void DeferredResources::image(AllocatedImage& value) {
    release(value.imageView, vkDestroyImageView);
    image(value.image, value.allocation);
    value = {};
}

AllocatedBuffer createBuffer(VmaAllocator allocator, VkDeviceSize size,
    VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    AllocatedBuffer result{};

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memoryUsage;
    if (memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU || memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    if (gpuCreateBuffer(allocator, &bufInfo, &allocInfo,
            &result.buffer, &result.allocation, &result.info) != VK_SUCCESS) {
        LOG_ERROR("Failed to create VMA buffer (size=", size, ")");
    }

    return result;
}

void destroyBuffer(VmaAllocator allocator, AllocatedBuffer& buffer) {
    destroy(allocator, buffer.buffer, buffer.allocation);
}

AllocatedImage createImage(VkDevice device, VmaAllocator allocator,
    uint32_t width, uint32_t height, VkFormat format,
    VkImageUsageFlags usage, VkSampleCountFlagBits samples, uint32_t mipLevels,
    std::source_location where) {
    AllocatedImage result{};
    result.extent = {.width = width, .height = height};
    result.format = format;

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent = {.width = width, .height = height, .depth = 1};
    imgInfo.mipLevels = mipLevels;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = samples;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = usage;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (VkResult r = gpuCreateImage(allocator, &imgInfo, &allocInfo,
            &result.image, &result.allocation, nullptr); r != VK_SUCCESS) {
        // What the driver had left says whether this is memory or something
        // else: the caller decides what the frame does without the image.
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(allocator, &props);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(allocator, budgets);
        uint64_t usage = 0, budget = 0;
        for (uint32_t i = 0; props && i < props->memoryHeapCount; ++i) {
            if ((props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
            if (budgets[i].budget > budget) {
                budget = budgets[i].budget;
                usage = budgets[i].usage;
            }
        }
        const char* file = where.file_name();
        if (const char* slash = std::strrchr(file, '/')) file = slash + 1;
        LOG_ERROR("Failed to create a ", width, "x", height, " image (format ", static_cast<int>(format),
                  ", ", mipLevels, " mips, ", static_cast<int>(samples), "x) for ", file, ":", where.line(),
                  ": ", vkResultName(r), " (", static_cast<int>(r), "); video memory ",
                  usage / (1024 * 1024), " MB in use of a ", budget / (1024 * 1024), " MB budget");
        return result;
    }

    // Name it by shape, so an allocation that survives shutdown can be traced
    // back from the leak dump to whatever asked for an image this size.
    {
        char name[64];
        const char* file = where.file_name();
        if (const char* slash = std::strrchr(file, '/')) file = slash + 1;
        std::snprintf(name, sizeof(name), "img %ux%u mips=%u fmt=%d from %s:%u",
                      width, height, mipLevels, static_cast<int>(format),
                      file, where.line());
        vmaSetAllocationName(allocator, result.allocation, name);
        setObjectName(device, VK_OBJECT_TYPE_IMAGE,
                      reinterpret_cast<uint64_t>(result.image), name);
    }

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;

    // Determine aspect mask from format
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM ||
        format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else {
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &result.imageView) != VK_SUCCESS) {
        LOG_ERROR("Failed to create image view");
    }

    return result;
}

void destroyImage(VkDevice device, VmaAllocator allocator, AllocatedImage& image) {
    if (image.imageView) {
        vkDestroyImageView(device, image.imageView, nullptr);
        image.imageView = VK_NULL_HANDLE;
    }
    if (image.image) {
        gpuDestroyImage(allocator, image.image, image.allocation);
        image.image = VK_NULL_HANDLE;
        image.allocation = VK_NULL_HANDLE;
    }
}

namespace {

/// The device's vkCmdPipelineBarrier2KHR, or nullptr for the legacy path.
PFN_vkCmdPipelineBarrier2KHR gPipelineBarrier2 = nullptr;

/// vkSetDebugUtilsObjectNameEXT, or nullptr when validation is off.
PFN_vkSetDebugUtilsObjectNameEXT gSetObjectName = nullptr;

/// synchronization2 stage and access masks are 64 bits and the legacy ones
/// are 32. The bits above 32 are the stages synchronization2 added and have
/// no legacy spelling, so a mask that uses one is widened to the coarsest
/// legacy equivalent instead. That waits for more than it needs, never less.
VkPipelineStageFlags lowerStages(VkPipelineStageFlags2 stages, bool isSource) {
    if ((stages >> 32) != 0) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (stages == 0) {
        // VK_PIPELINE_STAGE_2_NONE is legal; a legacy mask of zero is not.
        return isSource ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                        : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    return static_cast<VkPipelineStageFlags>(stages);
}

VkAccessFlags lowerAccess(VkAccessFlags2 access) {
    if ((access >> 32) != 0) {
        return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
    return static_cast<VkAccessFlags>(access);
}

} // namespace

void setPipelineBarrier2Fn(PFN_vkCmdPipelineBarrier2KHR fn) {
    gPipelineBarrier2 = fn;
}

void setObjectNameFn(PFN_vkSetDebugUtilsObjectNameEXT fn) {
    gSetObjectName = fn;
}

bool isObjectNamingActive() { return gSetObjectName != nullptr; }

void setObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name) {
    if (gSetObjectName == nullptr || handle == 0) return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    gSetObjectName(device, &info);
}

void cmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo& dep) {
    if (gPipelineBarrier2 != nullptr) {
        gPipelineBarrier2(cmd, &dep);
        return;
    }

    VkPipelineStageFlags2 srcStages = 0;
    VkPipelineStageFlags2 dstStages = 0;

    std::vector<VkMemoryBarrier> mems;
    mems.reserve(dep.memoryBarrierCount);
    for (uint32_t i = 0; i < dep.memoryBarrierCount; ++i) {
        const VkMemoryBarrier2& b = dep.pMemoryBarriers[i];
        srcStages |= b.srcStageMask;
        dstStages |= b.dstStageMask;
        mems.push_back({.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = lowerAccess(b.srcAccessMask),
                        .dstAccessMask = lowerAccess(b.dstAccessMask)});
    }

    std::vector<VkBufferMemoryBarrier> bufs;
    bufs.reserve(dep.bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < dep.bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier2& b = dep.pBufferMemoryBarriers[i];
        srcStages |= b.srcStageMask;
        dstStages |= b.dstStageMask;
        bufs.push_back({.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = lowerAccess(b.srcAccessMask),
                        .dstAccessMask = lowerAccess(b.dstAccessMask),
                        .srcQueueFamilyIndex = b.srcQueueFamilyIndex,
                        .dstQueueFamilyIndex = b.dstQueueFamilyIndex,
                        .buffer = b.buffer,
                        .offset = b.offset,
                        .size = b.size});
    }

    std::vector<VkImageMemoryBarrier> imgs;
    imgs.reserve(dep.imageMemoryBarrierCount);
    for (uint32_t i = 0; i < dep.imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2& b = dep.pImageMemoryBarriers[i];
        srcStages |= b.srcStageMask;
        dstStages |= b.dstStageMask;
        imgs.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = lowerAccess(b.srcAccessMask),
                        .dstAccessMask = lowerAccess(b.dstAccessMask),
                        .oldLayout = b.oldLayout,
                        .newLayout = b.newLayout,
                        .srcQueueFamilyIndex = b.srcQueueFamilyIndex,
                        .dstQueueFamilyIndex = b.dstQueueFamilyIndex,
                        .image = b.image,
                        .subresourceRange = b.subresourceRange});
    }

    vkCmdPipelineBarrier(cmd,
                         lowerStages(srcStages, true),
                         lowerStages(dstStages, false),
                         dep.dependencyFlags,
                         static_cast<uint32_t>(mems.size()), mems.empty() ? nullptr : mems.data(),
                         static_cast<uint32_t>(bufs.size()), bufs.empty() ? nullptr : bufs.data(),
                         static_cast<uint32_t>(imgs.size()), imgs.empty() ? nullptr : imgs.data());
}

void transitionImageLayout(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    // The stage masks are the caller's legacy flags widened. synchronization2
    // defines its first 32 stage and access bits to the same values, so a
    // VK_PIPELINE_STAGE_* constant is already a valid VK_PIPELINE_STAGE_2_*
    // one and no call site has to change.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.dstStageMask = dstStage;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    // Set access masks based on layouts
    switch (oldLayout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            barrier.srcAccessMask = 0;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            break;
        default:
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            break;
    }

    switch (newLayout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            barrier.dstAccessMask = 0;
            break;
        default:
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            break;
    }

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    cmdPipelineBarrier2(cmd, dep);
}

AllocatedBuffer uploadBuffer(VkContext& ctx, const void* data, VkDeviceSize size,
    VkBufferUsageFlags usage)
{
    // Create staging buffer
    AllocatedBuffer staging = createBuffer(ctx.getAllocator(), size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    // Copy data to staging
    void* mapped;
    vmaMapMemory(ctx.getAllocator(), staging.allocation, &mapped);
    std::memcpy(mapped, data, size);
    vmaUnmapMemory(ctx.getAllocator(), staging.allocation);

    // Create GPU buffer
    AllocatedBuffer gpuBuffer = createBuffer(ctx.getAllocator(), size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    // Copy staging -> GPU
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(cmd, staging.buffer, gpuBuffer.buffer, 1, &copyRegion);
    });

    // Destroy staging buffer (deferred if in batch mode)
    if (ctx.isInUploadBatch()) {
        ctx.deferStagingCleanup(staging);
    } else {
        destroyBuffer(ctx.getAllocator(), staging);
    }

    return gpuBuffer;
}

bool recordBufferUpload(VkContext& ctx, VkCommandBuffer cmd, ::VkBuffer target,
                        const void* data, VkDeviceSize size) {
    if (!cmd || !target || size == 0) return false;
    auto staging = createBuffer(ctx.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VMA_MEMORY_USAGE_CPU_TO_GPU);
    if (!staging.buffer || !staging.info.pMappedData) {
        destroyBuffer(ctx.getAllocator(), staging);
        return false;
    }
    std::memcpy(staging.info.pMappedData, data, static_cast<size_t>(size));
    vmaFlushAllocation(ctx.getAllocator(), staging.allocation, 0, size);
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = target;
    barrier.size = size;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &barrier;
    cmdPipelineBarrier2(cmd, dep);
    const VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.buffer, target, 1, &copy);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    cmdPipelineBarrier2(cmd, dep);
    ctx.deferAfterAllFrameFences([alloc = ctx.getAllocator(), staging]() mutable { destroyBuffer(alloc, staging); });
    return true;
}

bool uploadIntoBuffer(VkContext& ctx, const void* data, VkDeviceSize size,
                      VkBuffer target, VkDeviceSize targetOffset) {
    if (target == VK_NULL_HANDLE || size == 0) return false;

    AllocatedBuffer staging = createBuffer(ctx.getAllocator(), size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    if (staging.buffer == VK_NULL_HANDLE) return false;

    void* mapped = nullptr;
    if (vmaMapMemory(ctx.getAllocator(), staging.allocation, &mapped) != VK_SUCCESS) {
        destroyBuffer(ctx.getAllocator(), staging);
        return false;
    }
    std::memcpy(mapped, data, size);
    vmaUnmapMemory(ctx.getAllocator(), staging.allocation);

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{};
        region.dstOffset = targetOffset;
        region.size = size;
        vkCmdCopyBuffer(cmd, staging.buffer, target, 1, &region);
    });

    // See the header: inside a batch the copy is only recorded, so the staging
    // buffer goes to the batch, which frees it once the copy has run. Handed
    // to the batch it is also what makes the batch submit at all: a batch
    // with no staging buffers of its own is thrown away unsubmitted, and a
    // batch holding only copies made here lost every one of them.
    if (ctx.isInUploadBatch()) {
        ctx.deferStagingCleanup(staging);
    } else {
        destroyBuffer(ctx.getAllocator(), staging);
    }
    return true;
}

} // namespace rendering
} // namespace wowee
