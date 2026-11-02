#pragma once

// Where animated models' skeletons live on the GPU, the only place they are
// posed (skeleton.comp.glsl): a model's bones and every key of their tracks,
// packed into one blob of 32-bit words in a page of the pool, uploaded when
// the model loads and let go with it.
//
// A blob, in words from its first:
//   header   boneCount, maxDepth, bones, globalCount, globals
//   bones    16 words each: pivot xyz, parent, flags, keyBoneId, arm
//            (0 left, 1 right, -1 neither: the shoulder's key bone and what
//            hangs from it), depth (0 at a root), then the word of its
//            translation, rotation and scale track (~0 for none)
//   track    globalSequence, interpolation, sequence count, then for each
//            sequence its key count, the word of its times and of its values
//   times    one word each, milliseconds
//   values   four words each: a vec3 in xyz, a quaternion as xyzw
//   globals  each global sequence's duration in milliseconds
// Keys are cleaned as they are packed, as m2_track_sampler.hpp cleaned them
// when it read them: a vec3 that is not finite is the track's default, a
// rotation is a unit quaternion or the identity. A key array is cut to the
// shorter of its times and values.
//
// Pages hold many blobs; one too large for a page gets a page of its own
// size. A blob's words come back after the frames that may read them, handed
// back by the thread that retires frames and freed on the main thread before
// it next places one.

#include "rendering/range_allocator.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace wowee {
namespace pipeline {
struct M2Model;
}
namespace rendering {

class VkContext;

/// A model's bones and keys in the blob layout above; maxDepth is the
/// deepest bone's depth.
struct PackedSkeleton {
    std::vector<uint32_t> words;
    uint32_t boneCount = 0;
    uint32_t maxDepth = 0;
};
/// `boneLimit` bones at most, as the renderer draws them.
[[nodiscard]] PackedSkeleton packSkeleton(const pipeline::M2Model& model, uint32_t boneLimit);

class SkeletonPool {
public:
    /// A model's place: its page and first word; page -1 for none.
    struct Skeleton {
        int32_t page = -1;
        uint32_t first = 0;
        uint32_t words = 0;
        uint32_t boneCount = 0;
        [[nodiscard]] bool placed() const { return page >= 0; }
    };
    /// The shader binds this many pages; a pool that needs more says so.
    static constexpr uint32_t kMaxPages = 64;

    SkeletonPool() = default;
    ~SkeletonPool();
    SkeletonPool(const SkeletonPool&) = delete;
    SkeletonPool& operator=(const SkeletonPool&) = delete;

    void initialize(VkContext& context) { context_ = &context; }
    /// Every page let go, freed once no frame in flight reads it.
    void destroy();

    /// The model packed and copied into a page, inside an upload batch. An
    /// unplaced skeleton, said at warning level, when there is nowhere for it:
    /// the model is then drawn in its bind pose.
    [[nodiscard]] Skeleton place(const pipeline::M2Model& model, uint32_t boneLimit, const char* name);
    /// Its words back once no frame in flight can read them.
    void release(const Skeleton& skeleton);

    /// The page buffers, by page; null where there is none.
    [[nodiscard]] std::vector<::VkBuffer> pageBuffers() const;
    /// Changes whenever a page comes or goes, so a set naming them is
    /// rewritten.
    [[nodiscard]] uint32_t version() const { return version_; }

private:
    struct Page {
        AllocatedBuffer buffer;
        RangeAllocator words;
        bool fitted = false;
    };
    struct Returned {
        std::mutex mutex;
        std::vector<std::pair<std::shared_ptr<Page>, Skeleton>> skeletons;
    };
    void takeReturned();

    static constexpr uint32_t kPageWords = 1u << 22u;  // 16 MiB
    VkContext* context_ = nullptr;
    std::vector<std::shared_ptr<Page>> pages_;
    std::shared_ptr<Returned> returned_ = std::make_shared<Returned>();
    uint32_t version_ = 1;
    bool fullSaid_ = false;
};

} // namespace rendering
} // namespace wowee
