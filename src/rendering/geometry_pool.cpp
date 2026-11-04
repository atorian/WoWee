#include "rendering/geometry_pool.hpp"

#include "core/logger.hpp"
#include "rendering/retired_pages.hpp"
#include "rendering/vk_context.hpp"

#include <algorithm>

namespace wowee {
namespace rendering {

GeometryPool::~GeometryPool() { destroy(); }

void GeometryPool::initialize(VkContext& context) {
    context_ = &context;
    // A shared page made ahead on a worker, and again each time it is taken:
    // the frame that fills the last page takes it rather than allocating.
    spare_.start(
        [allocator = context.getAllocator(), config = config_] {
            return makePageBuffers(allocator, config, config.pageVertices, config.pageIndices, false);
        },
        [allocator = context.getAllocator()](Page& page) {
            destroyBuffer(allocator, page.vertices);
            destroyBuffer(allocator, page.indices);
        });
}

void GeometryPool::destroy() {
    spare_.stop();
    if (!context_ || pages_.empty()) return;
    // A frame in flight may still draw from them.
    context_->deferAfterAllFrameFences([allocator = context_->getAllocator(), pages = std::move(pages_)] {
        for (const auto& page : pages) {
            if (!page) continue;
            destroyBuffer(allocator, page->vertices);
            destroyBuffer(allocator, page->indices);
        }
    });
    pages_.clear();
    exhausted_ = false;
}

std::shared_ptr<GeometryPool::Page> GeometryPool::makePageBuffers(VmaAllocator allocator, const Config& config,
                                                                  uint32_t vertexRoom, uint32_t indexRoom,
                                                                  bool fitted) {
    auto page = std::make_shared<Page>();
    const uint32_t indexBytes = config.indexType == VK_INDEX_TYPE_UINT16 ? 2u : 4u;
    page->vertices = createBuffer(allocator, VkDeviceSize(vertexRoom) * config.vertexStride,
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_GPU_ONLY);
    page->indices = createBuffer(allocator, VkDeviceSize(indexRoom) * indexBytes,
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VMA_MEMORY_USAGE_GPU_ONLY);
    if (!page->vertices.buffer || !page->indices.buffer) {
        if (page->vertices.buffer) destroyBuffer(allocator, page->vertices);
        if (page->indices.buffer) destroyBuffer(allocator, page->indices);
        return nullptr;
    }
    page->vertexRanges = RangeAllocator(vertexRoom);
    page->indexRanges = RangeAllocator(indexRoom);
    page->fitted = fitted;
    return page;
}

bool GeometryPool::makePage(uint32_t vertexRoom, uint32_t indexRoom, bool fitted, int32_t& slot) {
    std::shared_ptr<Page> page = makePageBuffers(context_->getAllocator(), config_, vertexRoom, indexRoom, fitted);
    if (!page) return false;
    slot = attach(std::move(page));
    return true;
}

int32_t GeometryPool::attach(std::shared_ptr<Page> page) {
    auto empty = std::find(pages_.begin(), pages_.end(), nullptr);
    if (empty == pages_.end()) {
        pages_.push_back(std::move(page));
        return static_cast<int32_t>(pages_.size() - 1);
    }
    *empty = std::move(page);
    return static_cast<int32_t>(empty - pages_.begin());
}

GeometryPool::Range GeometryPool::place(const void* vertices, uint32_t vertexCount, const void* indices,
                                        uint32_t indexCount) {
    if (vertexCount == 0 || indexCount == 0) return {};
    takeReturned();
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
        const bool first = !spare_.started();
        if (std::shared_ptr<Page> spare = spare_.take()) {
            slot = attach(std::move(spare));
            LOG_INFO(config_.what, ": geometry page ", slot, " taken from the spare (", stats().pages, " resident)");
        } else if (makePage(config_.pageVertices, config_.pageIndices, false, slot)) {
            if (first) {
                LOG_INFO(config_.what, ": first geometry page made; the next is made ahead on a worker");
            } else {
                // The frame allocated it: the spare was still being made, or
                // could not be.
                static core::LogBudget onFrame(16, "geometry pages made on the frame");
                if (onFrame.take())
                    LOG_WARNING(config_.what, ": geometry page ", slot, " made on the frame - its spare was not ready (",
                                stats().pages, " resident)");
            }
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
                 .vertexCount = vertexCount, .indexCount = indexCount, .vertices = page.vertices.buffer,
                 .indices = page.indices.buffer};
}

void GeometryPool::release(const Range& range) {
    if (!range.placed() || static_cast<size_t>(range.page) >= pages_.size() || !pages_[range.page]) return;
    // A frame in flight may still draw it; the ranges come back after it, on
    // the thread that retires frames, and are freed here on the next call.
    context_->deferAfterAllFrameFences([returned = returned_, page = pages_[range.page], range] {
        std::lock_guard lock(returned->mutex);
        returned->ranges.emplace_back(page, range);
    });
}

void GeometryPool::takeReturned() {
    std::vector<std::pair<std::shared_ptr<Page>, Range>> ranges;
    {
        std::lock_guard lock(returned_->mutex);
        ranges.swap(returned_->ranges);
    }
    for (const auto& [page, range] : ranges) {
        page->vertexRanges.free(static_cast<uint32_t>(range.baseVertex), range.vertexCount);
        page->indexRanges.free(range.firstIndex, range.indexCount);
    }
}

void GeometryPool::releaseEmptyPages() {
    takeReturned();
    // Every mesh in an empty page went back behind the frame fences, but a
    // bucket of draws binds the page whole, so it goes behind them as well.
    const auto empty = [](const Page& page) {
        return page.vertexRanges.used() == 0 && page.indexRanges.used() == 0;
    };
    const auto destroy = [](VmaAllocator allocator, Page& page) {
        destroyBuffer(allocator, page.vertices);
        destroyBuffer(allocator, page.indices);
    };
    if (retireEmptyPages(*context_, pages_, empty, destroy)) exhausted_ = false;
}

::VkBuffer GeometryPool::vertices(int32_t page) const {
    return page >= 0 && static_cast<size_t>(page) < pages_.size() && pages_[page] ? pages_[page]->vertices.buffer
                                                                                    : VK_NULL_HANDLE;
}

::VkBuffer GeometryPool::indices(int32_t page) const {
    return page >= 0 && static_cast<size_t>(page) < pages_.size() && pages_[page] ? pages_[page]->indices.buffer
                                                                                    : VK_NULL_HANDLE;
}

void GeometryPool::bind(VkCommandBuffer cmd, const Range& range) const {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &range.vertices, &offset);
    vkCmdBindIndexBuffer(cmd, range.indices, 0, config_.indexType);
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
