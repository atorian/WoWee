#pragma once

// Where a renderer's meshes live on the GPU: pages of one vertex buffer and
// one index buffer, many meshes each, a mesh at ranges handed out by
// RangeAllocators. A draw names a mesh by its page, its first index and its
// base vertex, so one indirect call can draw any of a page's meshes, and the
// buffers are bound once a page rather than once a mesh.
//
// A mesh no shared page has room for, and for which none can be made, gets a
// page of its own size in the same slots (a fitted page). Only when not even
// that can be made is it refused, said at warning level. A mesh's ranges come
// back after the frames that may draw them; a page left empty goes when its
// owner says so (releaseEmptyPages), all but one spare shared page.
//
// Main thread; the buffers are read by the recording.

#include "rendering/range_allocator.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

class GeometryPool {
public:
    struct Config {
        const char* what;           ///< names it in the log
        uint32_t vertexStride;
        VkIndexType indexType;      ///< 16- or 32-bit, relative to the mesh's base vertex
        uint32_t pageVertices;      ///< a shared page's room
        uint32_t pageIndices;
    };
    /// Where one mesh lives; page -1 for none.
    struct Range {
        int32_t page = -1;
        int32_t baseVertex = 0;
        uint32_t firstIndex = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        [[nodiscard]] bool placed() const { return page >= 0; }
    };
    struct Stats {
        uint32_t pages = 0;
        uint32_t fittedPages = 0;
        uint64_t vertexCapacity = 0, verticesUsed = 0;
        uint64_t indexCapacity = 0, indicesUsed = 0;
    };

    explicit GeometryPool(Config config) : config_(config) {}
    ~GeometryPool();
    GeometryPool(const GeometryPool&) = delete;
    GeometryPool& operator=(const GeometryPool&) = delete;

    void initialize(VkContext& context) { context_ = &context; }
    /// Every page freed now; the device must be idle and nothing drawn.
    void destroy();

    /// The mesh copied into a page, inside an upload batch. An unplaced
    /// range, said at warning level, when there is nowhere for it.
    [[nodiscard]] Range place(const void* vertices, uint32_t vertexCount, const void* indices, uint32_t indexCount);
    /// The mesh's ranges back once no frame in flight can draw them.
    void release(Range& range);
    /// Pages that emptied go, but for one shared spare.
    void releaseEmptyPages();

    [[nodiscard]] size_t pageCount() const { return pages_.size(); }
    /// The page's buffers; null for a slot with no page.
    [[nodiscard]] ::VkBuffer vertices(int32_t page) const;
    [[nodiscard]] ::VkBuffer indices(int32_t page) const;
    [[nodiscard]] VkIndexType indexType() const { return config_.indexType; }
    /// Binds the page's buffers at binding 0.
    void bind(VkCommandBuffer cmd, int32_t page) const;
    [[nodiscard]] Stats stats() const;

private:
    struct Page {
        AllocatedBuffer vertices;
        AllocatedBuffer indices;
        RangeAllocator vertexRanges;
        RangeAllocator indexRanges;
        bool fitted = false;
    };
    [[nodiscard]] uint32_t indexBytes() const { return config_.indexType == VK_INDEX_TYPE_UINT16 ? 2u : 4u; }
    bool makePage(uint32_t vertexRoom, uint32_t indexRoom, bool fitted, int32_t& slot);

    Config config_;
    VkContext* context_ = nullptr;
    std::vector<std::shared_ptr<Page>> pages_;
    /// Set, and said once, when a shared page could not be made: none is
    /// tried again until a page is let go.
    bool exhausted_ = false;
};

} // namespace rendering
} // namespace wowee
