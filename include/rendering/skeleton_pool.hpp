#pragma once

// Where animated models' skeletons and particle emitters live on the GPU,
// the only place they are posed and run (skeleton.comp.glsl,
// m2_particles.comp.glsl): a model's bones and every key of their tracks,
// packed into one blob of 32-bit words in a page of the pool, and its
// emitters into another, uploaded when the model loads and let go with it.
//
// A skeleton blob, in words from its first:
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
// A model's particle and ribbon emitters are a blob of their own
// (m2_particles.comp, m2_ribbons.comp):
//   header   particle emitter count, ribbon count, emitters, globalCount,
//            globals, ribbons, two words unused (8 words)
//   emitters kEmitterWords each: flags, type, bone, atlas columns | rows
//            << 16, position xyz, drag, tail length, head 1 | tail 2 |
//            enabled 4, scale variation xy, twinkle speed, percent, scale
//            min and max, base spin and its variation, spin and its
//            variation, wind xyz, wind time, life variation, rate
//            variation, capacity; from kEmitterTracks the word of each of
//            its float tracks - speed, speed variation, vertical and
//            horizontal range, gravity, life, rate, area length and width,
//            zSource (~0 for none); from kEmitterFBlocks the word of its
//            colour, alpha, scale, head cell and tail cell FBlocks
//   ribbons  kRibbonWords each: bone, position xyz, edges a second, an
//            edge's life, gravity, capacity; from kRibbonTracks the word of
//            its colour, alpha, height above and below and visibility tracks
//   tracks   as a skeleton's, a float in a value's first word
//   FBlocks  value count, time count, the times, four words a value
//   globals  as a skeleton's, at the same header words, so one sampler
//            (m2_track.glsli) reads both
//
// Pages hold many blobs; one too large for a page gets a page of its own
// size. A blob's words come back after the frames that may read them, handed
// back by the thread that retires frames and freed on the main thread before
// it next places one.
//
// A blob is held once however many models pack to it. The client loads a
// model once per creature display, and the displays that dress one race's
// model differently - every humanoid NPC - pack the same skeleton: held
// apart, Dalaran's took all 64 pages, a gigabyte, and every model loaded
// after that stood in its bind pose. A blob is known by its words alone, so
// models from one file share it whatever the display, path or name they were
// loaded under, and it goes when the last of them is let go.

#include "rendering/range_allocator.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
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

/// A model's texture animations (M2TextureTransform), as m2_draws.comp
/// samples them into a draw's UV transform:
///   header   transform count, 0, transforms, globalCount, globals - the
///            skeleton's header words, so m2_track.glsli reads it
///   transforms  3 words each: the word of its translation, rotation and
///            scale track (~0 for none)
///   tracks, globals  as a skeleton's
[[nodiscard]] std::vector<uint32_t> packTextureAnimations(const pipeline::M2Model& model);

/// The emitter blob's shape (above), as m2_particles.comp reads it.
inline constexpr uint32_t kEmitterHeaderWords = 8;
inline constexpr uint32_t kEmitterWords = 48;
inline constexpr uint32_t kEmitterTracks = 28;
inline constexpr uint32_t kEmitterFBlocks = 38;
inline constexpr uint32_t kRibbonWords = 16;
inline constexpr uint32_t kRibbonTracks = 8;
/// Room for this many particles at most, an emitter: one that may hold more
/// emits no more than this, said when it is packed.
inline constexpr uint32_t kMaxEmitterParticles = 16384;

/// A model's particle and ribbon emitters in the blob layout above, the
/// most particles each particle emitter may hold and the most edges each
/// ribbon (their capacity words).
struct PackedEmitters {
    std::vector<uint32_t> words;
    std::vector<uint32_t> capacities;
    std::vector<uint32_t> ribbonCapacities;
};
[[nodiscard]] PackedEmitters packEmitters(const pipeline::M2Model& model);

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
    /// A model's emitters' place, as a skeleton's, and the most particles
    /// each particle emitter may hold and the most edges each ribbon.
    struct Emitters {
        int32_t page = -1;
        uint32_t first = 0;
        uint32_t words = 0;
        std::vector<uint32_t> capacities;
        std::vector<uint32_t> ribbonCapacities;
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
    /// The model's particle and ribbon emitters packed and copied, the same
    /// way. None placed, said, when there is nowhere for them: it is drawn
    /// without its particles and ribbons.
    [[nodiscard]] Emitters placeEmitters(const pipeline::M2Model& model, const char* name);
    /// The model's texture animations packed and copied, the same way; a
    /// Skeleton whose boneCount is the transform count. None placed, said,
    /// when there is nowhere for them: its textures then stand still.
    [[nodiscard]] Skeleton placeTextureAnimations(const pipeline::M2Model& model, const char* name);
    /// A blob packed elsewhere (a model's draw list, m2_draw_pass.hpp),
    /// copied the same way; none placed, said with what `name` is `without`
    /// then, when there is nowhere for it.
    [[nodiscard]] Skeleton placeBlob(const std::vector<uint32_t>& words, const char* name, const char* without);
    /// Let go by one model; its words come back once the last model holding
    /// them has let go and no frame in flight can read them.
    void release(const Skeleton& skeleton);
    void release(const Emitters& emitters);

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
        struct Blob {
            std::shared_ptr<Page> page;
            uint32_t first = 0;
            uint32_t words = 0;
        };
        std::mutex mutex;
        std::vector<Blob> blobs;
    };
    void takeReturned();
    /// A blob copied into a page: its page and first word; none, said with
    /// what the model is `without` then, when there is nowhere for it.
    std::optional<std::pair<int32_t, uint32_t>> placeWords(const std::vector<uint32_t>& blob, const char* name,
                                                          const char* without);
    void releaseWords(int32_t page, uint32_t first, uint32_t words);
    /// The blob copied into a page, without looking for one held already.
    std::optional<std::pair<int32_t, uint32_t>> copyWords(const std::vector<uint32_t>& blob, const char* name,
                                                         const char* without);

    /// A blob's words told apart from any other's: two independent 64-bit
    /// digests of them and how many there are.
    struct Content {
        uint64_t a = 0;
        uint64_t b = 0;
        uint32_t words = 0;
        bool operator==(const Content&) const = default;
    };
    struct ContentHash {
        size_t operator()(const Content& c) const noexcept { return static_cast<size_t>(c.a); }
    };
    [[nodiscard]] static Content contentOf(const std::vector<uint32_t>& blob);
    /// A placed blob and how many models hold it.
    struct Held {
        int32_t page = -1;
        uint32_t first = 0;
        uint32_t users = 0;
    };
    std::unordered_map<Content, Held, ContentHash> held_;
    /// The content at a page's word, for the release that names only where.
    std::unordered_map<uint64_t, Content> contentAt_;

    static constexpr uint32_t kPageWords = 1u << 22u;  // 16 MiB
    VkContext* context_ = nullptr;
    std::vector<std::shared_ptr<Page>> pages_;
    std::shared_ptr<Returned> returned_ = std::make_shared<Returned>();
    uint32_t version_ = 1;
    bool fullSaid_ = false;
};

} // namespace rendering
} // namespace wowee
