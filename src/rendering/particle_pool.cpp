#include "rendering/particle_pool.hpp"

#include "foundation/logger.hpp"
#include "rendering/retired_pages.hpp"
#include "rendering/vk_context.hpp"

#include <algorithm>

namespace wowee {
namespace rendering {

ParticlePool::~ParticlePool() { destroy(); }

void ParticlePool::destroy() {
    if (!context_ || pages_.empty()) return;
    // A frame in flight may still run them.
    retirePages(*context_, pages_);
    ++version_;
}

void ParticlePool::takeReturned() {
    std::vector<Returned::Records> ranges;
    {
        std::lock_guard lock(returned_->mutex);
        ranges.swap(returned_->ranges);
    }
    for (const auto& range : ranges) range.page->records.free(range.first, range.count);
    // A page left empty goes, but for one shared one.
    if (retireEmptyPages(
            *context_, pages_, [](const Page& page) { return page.records.used() == 0; },
            [](VmaAllocator allocator, Page& page) { destroyBuffer(allocator, page.buffer); }))
        ++version_;
}

ParticlePool::Range ParticlePool::place(uint32_t capacity, const char* name) {
    if (!context_ || capacity == 0) return {};
    takeReturned();
    const uint32_t records = capacity + 1;  // and its header

    int32_t slot = -1;
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i] && !pages_[i]->fitted && pages_[i]->records.largestFree() >= records) {
            slot = static_cast<int32_t>(i);
            break;
        }
    }
    if (slot < 0) {
        const bool fitted = records > kPageRecords;
        auto empty = std::find(pages_.begin(), pages_.end(), nullptr);
        const auto at = static_cast<size_t>(empty - pages_.begin());
        if (at >= kMaxPages) {
            if (!fullSaid_) {
                LOG_WARNING("Particle pool: all ", kMaxPages, " pages in use - '", name,
                            "' and the emitters after it emit nothing until a page is let go");
                fullSaid_ = true;
            }
            return {};
        }
        auto page = std::make_shared<Page>();
        const uint32_t room = fitted ? records : kPageRecords;
        page->buffer = createBuffer(context_->getAllocator(), VkDeviceSize(room) * kRecordWords * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
        if (!page->buffer.buffer) {
            LOG_WARNING("Particle pool: no memory for a page of ", room, " particles - '", name, "' emits nothing");
            return {};
        }
        page->records = RangeAllocator(room);
        page->fitted = fitted;
        if (empty == pages_.end()) pages_.push_back(std::move(page));
        else *empty = std::move(page);
        slot = static_cast<int32_t>(at);
        fullSaid_ = false;
        ++version_;
    }
    const auto first = pages_[static_cast<size_t>(slot)]->records.allocate(records);
    if (!first) {
        LOG_WARNING("Particle pool: no room for ", records, " records - '", name, "' emits nothing");
        return {};
    }
    // Never 0, so a page's cleared records are no range's header.
    const uint32_t tag = nextTag_++;
    if (nextTag_ == 0) nextTag_ = 1;
    return Range{.page = slot, .first = *first, .capacity = capacity, .tag = tag};
}

void ParticlePool::release(const Range& range) {
    if (!range.placed() || static_cast<size_t>(range.page) >= pages_.size() || !pages_[range.page]) return;
    context_->deferAfterAllFrameFences([returned = returned_, page = pages_[range.page], first = range.first,
                                        count = range.capacity + 1] {
        std::lock_guard lock(returned->mutex);
        returned->ranges.push_back({.page = page, .first = first, .count = count});
    });
}

std::vector<::VkBuffer> ParticlePool::pageBuffers() const {
    std::vector<::VkBuffer> buffers(pages_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < pages_.size(); ++i)
        if (pages_[i]) buffers[i] = pages_[i]->buffer.buffer;
    return buffers;
}

} // namespace rendering
} // namespace wowee
