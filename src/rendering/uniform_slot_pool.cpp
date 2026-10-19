#include "rendering/uniform_slot_pool.hpp"

#include "core/logger.hpp"

#include <algorithm>

namespace wowee {
namespace rendering {

UniformSlotPool::UniformSlotPool(VkDeviceSize slotBytes, VkDeviceSize alignment, uint32_t slotsPerBlock,
                                 std::string name, MakeBlock make, FreeBlock free)
    : slotBytes_(((std::max<VkDeviceSize>(slotBytes, 1) + std::max<VkDeviceSize>(alignment, 1) - 1) /
                  std::max<VkDeviceSize>(alignment, 1)) * std::max<VkDeviceSize>(alignment, 1)),
      slotsPerBlock_(std::max(slotsPerBlock, 1u)),
      name_(std::move(name)),
      make_(std::move(make)),
      freeBlock_(std::move(free)) {}

UniformSlotPool::~UniformSlotPool() { release(); }

UniformSlotPool::Slot UniformSlotPool::allocate() {
    std::lock_guard lock(mutex_);
    if (released_) return {};
    if (freeSlots_.empty()) {
        std::optional<Block> block = make_(slotBytes_ * slotsPerBlock_);
        if (!block) {
            if (!failureSaid_) {
                failureSaid_ = true;
                LOG_ERROR(name_, ": could not make a block of ", slotsPerBlock_, " uniform slots (",
                          slotBytes_ * slotsPerBlock_, " bytes) with ", inUse_,
                          " in use - what asks for one now draws without its parameters");
            }
            return {};
        }
        const uint32_t first = static_cast<uint32_t>(blocks_.size()) * slotsPerBlock_;
        blocks_.push_back(*block);
        // Highest first, so the block fills from its start.
        for (uint32_t i = slotsPerBlock_; i-- > 0;) freeSlots_.push_back(first + i);
    }
    const uint32_t id = freeSlots_.back();
    freeSlots_.pop_back();
    ++inUse_;
    const Block& block = blocks_[id / slotsPerBlock_];
    const VkDeviceSize offset = static_cast<VkDeviceSize>(id % slotsPerBlock_) * slotBytes_;
    return Slot{.buffer = block.buffer,
                .offset = offset,
                .mapped = static_cast<uint8_t*>(block.mapped) + offset,
                .id = id};
}

void UniformSlotPool::free(const Slot& slot) {
    if (!slot) return;
    std::lock_guard lock(mutex_);
    if (released_ || slot.id / slotsPerBlock_ >= blocks_.size()) return;
    freeSlots_.push_back(slot.id);
    --inUse_;
}

void UniformSlotPool::release() {
    std::lock_guard lock(mutex_);
    if (released_) return;
    released_ = true;
    for (const Block& block : blocks_) freeBlock_(block);
    blocks_.clear();
    freeSlots_.clear();
    inUse_ = 0;
}

size_t UniformSlotPool::blockCount() const {
    std::lock_guard lock(mutex_);
    return blocks_.size();
}

size_t UniformSlotPool::slotsInUse() const {
    std::lock_guard lock(mutex_);
    return inUse_;
}

} // namespace rendering
} // namespace wowee
