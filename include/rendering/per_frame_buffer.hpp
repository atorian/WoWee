#pragma once

#include "rendering/frames_in_flight.hpp"
#include "rendering/vk_utils.hpp"

#include <array>

namespace wowee {
namespace rendering {

/// A host-visible buffer the CPU rewrites every frame (streamed particle
/// vertices and the like), one copy per frame in flight. The frame being
/// recorded writes and binds its own slot's copy; the GPU may still be
/// reading the other one for the frame before.
class PerFrameBuffer {
public:
    /// Every copy `size` bytes, mapped. False, with nothing kept, when one of
    /// them cannot be made.
    bool create(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
        destroy(allocator);
        for (AllocatedBuffer& copy : copies_) {
            copy = createBuffer(allocator, size, usage, VMA_MEMORY_USAGE_CPU_TO_GPU);
            if (copy.buffer == VK_NULL_HANDLE || !copy.info.pMappedData) {
                destroy(allocator);
                return false;
            }
        }
        size_ = size;
        return true;
    }

    void destroy(VmaAllocator allocator) {
        for (AllocatedBuffer& copy : copies_) {
            if (copy.buffer) destroyBuffer(allocator, copy);
            copy = {};
        }
        size_ = 0;
    }

    bool valid() const { return size_ != 0; }
    VkDeviceSize size() const { return size_; }

    /// The copy for frame slot `frame` (`VkContext::getCurrentFrame()`).
    VkBuffer buffer(uint32_t frame) const { return copies_[frame % MAX_FRAMES_IN_FLIGHT].buffer; }
    void* mapped(uint32_t frame) const {
        return copies_[frame % MAX_FRAMES_IN_FLIGHT].info.pMappedData;
    }

private:
    std::array<AllocatedBuffer, MAX_FRAMES_IN_FLIGHT> copies_{};
    VkDeviceSize size_ = 0;
};

} // namespace rendering
} // namespace wowee
