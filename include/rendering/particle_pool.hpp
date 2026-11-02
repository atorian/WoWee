#pragma once

// Where the M2 particles live: on the GPU only, run and drawn there
// (m2_particles.comp.glsl). Each running emitter of an instance has a range
// of records in a page of the pool - a header and then room for as many
// particles as the emitter may ever hold - kept from the first frame it runs
// until its instance goes, as its particles were kept with the instance.
//
// A record is 16 words. The header: the range's tag (a range the shader
// finds under another tag is fresh, and it clears it first), the
// accumulated spawn fraction, the pool the emitter has grown to, how many
// particles it has spawned, how many are alive and how far the farthest is
// from its instance. A particle: position xyz and age; velocity xyz and
// life (none alive at or under zero); size factor xy, spin draws; spin
// sign, twinkle slot, atlas cell.
//
// Pages hold many ranges; one too large for a page gets a page of its own
// size. A range comes back after the frames that may run it, handed back by
// the thread that retires frames and freed on the main thread before it next
// places one.

#include "rendering/range_allocator.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

class ParticlePool {
public:
    /// An emitter's records: its page, its header's record, how many
    /// particles it has room for, and the tag the shader knows it by.
    struct Range {
        int32_t page = -1;
        uint32_t first = 0;
        uint32_t capacity = 0;
        uint32_t tag = 0;
        [[nodiscard]] bool placed() const { return page >= 0; }
    };
    /// The shader binds this many pages; a pool that needs more says so.
    static constexpr uint32_t kMaxPages = 64;
    static constexpr uint32_t kRecordWords = 16;

    ParticlePool() = default;
    ~ParticlePool();
    ParticlePool(const ParticlePool&) = delete;
    ParticlePool& operator=(const ParticlePool&) = delete;

    void initialize(VkContext& context) { context_ = &context; }
    /// Every page let go, freed once no frame in flight reads it.
    void destroy();

    /// Room for `capacity` particles and their header. None placed, said
    /// once per `name`, when there is no room: the emitter emits nothing.
    [[nodiscard]] Range place(uint32_t capacity, const char* name);
    /// Its records back once no frame in flight can run them.
    void release(const Range& range);

    /// The page buffers, by page; null where there is none.
    [[nodiscard]] std::vector<::VkBuffer> pageBuffers() const;
    /// Changes whenever a page comes or goes, so a set naming them is
    /// rewritten.
    [[nodiscard]] uint32_t version() const { return version_; }

private:
    struct Page {
        AllocatedBuffer buffer;
        RangeAllocator records;
        bool fitted = false;
    };
    struct Returned {
        struct Records {
            std::shared_ptr<Page> page;
            uint32_t first = 0;
            uint32_t count = 0;
        };
        std::mutex mutex;
        std::vector<Records> ranges;
    };
    void takeReturned();

    static constexpr uint32_t kPageRecords = 1u << 16u;  // 4 MiB
    VkContext* context_ = nullptr;
    std::vector<std::shared_ptr<Page>> pages_;
    std::shared_ptr<Returned> returned_ = std::make_shared<Returned>();
    uint32_t version_ = 1;
    uint32_t nextTag_ = 1;
    bool fullSaid_ = false;
};

} // namespace rendering
} // namespace wowee
