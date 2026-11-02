#include "rendering/geometry_pool.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"

#include <algorithm>

namespace wowee {
namespace rendering {

GeometryPool::~GeometryPool() { destroy(); }

void GeometryPool::destroy() {
    if (!context_) return;
    VmaAllocator allocator = context_->getAllocator();
    for (auto& page : pages_) {
        if (!page) continue;
        destroyBuffer(allocator, page->vertices);
        destroyBuffer(allocator, page->indices);
    }
    pages_.clear();
    exhausted_ = false;
}

bool GeometryPool::makePage(uint32_t vertexRoom, uint32_t indexRoom, bool fitted, int32_t& slot) {
    auto page = std::make_shared<Page>();
    VmaAllocator allocator = context_->getAllocator();
    page->vertices = createBuffer(allocator, VkDeviceSize(vertexRoom) * config_.vertexStride,
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_GPU_ONLY);
    page->indices = createBuffer(allocator, VkDeviceSize(indexRoom) * indexBytes(),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VMA_MEMORY_USAGE_GPU_ONLY);
    if (!page->vertices.buffer || !page->indices.buffer) {
        if (page->vertices.buffer) destroyBuffer(allocator, page->vertices);
        if (page->indices.buffer) destroyBuffer(allocator, page->indices);
        return false;
    }
    page->vertexRanges = RangeAllocator(vertexRoom);
    page->indexRanges = RangeAllocator(indexRoom);
    page->fitted = fitted;
    auto empty = std::find(pages_.begin(), pages_.end(), nullptr);
    if (empty == pages_.end()) {
        pages_.push_back(std::move(page));
        slot = static_cast<int32_t>(pages_.size() - 1);
    } else {
        *empty = std::move(page);
        slot = static_cast<int32_t>(empty - pages_.begin());
    }
    return true;
}

GeometryPool::Range GeometryPool::place(const void* vertices, uint32_t vertexCount, const void* indices,
                                        uint32_t indexCount) {
    if (vertexCount == 0 || indexCount == 0) return {};
    const bool shareable = vertexCount <= config_.pageVertices && indexCount <= config_.pageIndices;

    // A shared page with room, else a new one in the first empty slot, else
    // a page of the mesh's own size.
    int32_t slot = -1;
    for (size_t i = 0; shareable && i < pages_.size(); ++i) {
        const Page* page = pages_[i].get();
        if (!page || page->fitted || page->vertexRanges.largestFree() < vertexCount ||
            page->indexRanges.largestFree() < indexCount) {
            continue;
        }
        slot = static_cast<int32_t>(i);
        break;
    }
    if (slot < 0 && shareable && !exhausted_) {
        if (makePage(config_.pageVertices, config_.pageIndices, false, slot)) {
            LOG_INFO(config_.what, ": geometry page ", slot, " made (", stats().pages, " resident)");
        } else {
            exhausted_ = true;
            LOG_WARNING(config_.what, ": no geometry page could be allocated (", stats().pages,
                        " pages resident) - meshes get pages of their own size until a page is let go");
        }
    }
    if (slot < 0 && !makePage(vertexCount, indexCount, true, slot)) {
        static core::LogBudget budget(4, "geometry pages");
        if (budget.take())
            LOG_WARNING(config_.what, ": no memory for a mesh's own geometry page (", vertexCount, " vertices, ",
                        indexCount, " indices) - it is not drawn");
        return {};
    }

    Page& page = *pages_[static_cast<size_t>(slot)];
    const auto firstVertex = page.vertexRanges.allocate(vertexCount);
    const auto firstIndex = page.indexRanges.allocate(indexCount);
    const bool copied =
        firstVertex && firstIndex &&
        uploadIntoBuffer(*context_, vertices, VkDeviceSize(vertexCount) * config_.vertexStride, page.vertices.buffer,
                         VkDeviceSize(*firstVertex) * config_.vertexStride) &&
        uploadIntoBuffer(*context_, indices, VkDeviceSize(indexCount) * indexBytes(), page.indices.buffer,
                         VkDeviceSize(*firstIndex) * indexBytes());
    if (!copied) {
        // Nothing has drawn these ranges, so they come back at once.
        if (firstVertex) page.vertexRanges.free(*firstVertex, vertexCount);
        if (firstIndex) page.indexRanges.free(*firstIndex, indexCount);
        LOG_WARNING(config_.what, ": a mesh's geometry could not be staged into page ", slot, " - it is not drawn");
        return {};
    }
    return Range{.page = slot, .baseVertex = static_cast<int32_t>(*firstVertex), .firstIndex = *firstIndex,
                 .vertexCount = vertexCount, .indexCount = indexCount};
}

void GeometryPool::release(Range& range) {
    if (!range.placed() || static_cast<size_t>(range.page) >= pages_.size() || !pages_[range.page]) {
        range = {};
        return;
    }
    // A frame in flight may still draw it; the ranges come back after it.
    context_->deferAfterAllFrameFences([page = pages_[range.page], range] {
        page->vertexRanges.free(static_cast<uint32_t>(range.baseVertex), range.vertexCount);
        page->indexRanges.free(range.firstIndex, range.indexCount);
    });
    range = {};
}

void GeometryPool::releaseEmptyPages() {
    // A page is empty only once every mesh in it has been retired behind the
    // frame fences, so nothing in flight reads it and it can go now.
    bool spareKept = false;
    VmaAllocator allocator = context_->getAllocator();
    for (auto& page : pages_) {
        if (!page || page->vertexRanges.used() != 0 || page->indexRanges.used() != 0) continue;
        if (!spareKept && !page->fitted) {
            spareKept = true;
            continue;
        }
        destroyBuffer(allocator, page->vertices);
        destroyBuffer(allocator, page->indices);
        page.reset();
        exhausted_ = false;
    }
    while (!pages_.empty() && !pages_.back()) pages_.pop_back();
}

::VkBuffer GeometryPool::vertices(int32_t page) const {
    return page >= 0 && static_cast<size_t>(page) < pages_.size() && pages_[page] ? pages_[page]->vertices.buffer
                                                                                    : VK_NULL_HANDLE;
}

::VkBuffer GeometryPool::indices(int32_t page) const {
    return page >= 0 && static_cast<size_t>(page) < pages_.size() && pages_[page] ? pages_[page]->indices.buffer
                                                                                    : VK_NULL_HANDLE;
}

void GeometryPool::bind(VkCommandBuffer cmd, int32_t page) const {
    const ::VkBuffer vertexBuffer = vertices(page);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, indices(page), 0, config_.indexType);
}

GeometryPool::Stats GeometryPool::stats() const {
    Stats stats;
    for (const auto& page : pages_) {
        if (!page) continue;
        ++stats.pages;
        if (page->fitted) ++stats.fittedPages;
        stats.vertexCapacity += page->vertexRanges.capacity();
        stats.verticesUsed += page->vertexRanges.used();
        stats.indexCapacity += page->indexRanges.capacity();
        stats.indicesUsed += page->indexRanges.used();
    }
    return stats;
}

} // namespace rendering
} // namespace wowee
