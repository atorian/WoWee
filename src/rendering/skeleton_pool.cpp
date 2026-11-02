#include "rendering/skeleton_pool.hpp"

#include "core/logger.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/vk_context.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace wowee {
namespace rendering {

namespace {

constexpr uint32_t kNone = ~0u;
constexpr uint32_t kBoneWords = 16;
// The shoulders' key bones: everything hanging from one is that arm.
constexpr int32_t kKeyBoneShoulderL = 2;
constexpr int32_t kKeyBoneShoulderR = 3;

uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }

enum class TrackKind { Vec3, Quat };

/// A track's record and its keys appended to `out`; its first word, or kNone
/// for a track with no keys.
uint32_t packTrack(const pipeline::M2AnimationTrack& track, TrackKind kind, const glm::vec3& fallback,
                   std::vector<uint32_t>& out) {
    if (!track.hasData()) return kNone;
    const auto record = static_cast<uint32_t>(out.size());
    const auto sequences = static_cast<uint32_t>(track.sequences.size());
    out.push_back(static_cast<uint32_t>(static_cast<int32_t>(track.globalSequence)));
    out.push_back(track.interpolationType);
    out.push_back(sequences);
    out.resize(out.size() + size_t(3) * sequences, 0u);
    for (uint32_t s = 0; s < sequences; ++s) {
        const auto& keys = track.sequences[s];
        const size_t values = kind == TrackKind::Vec3 ? keys.vec3Values.size() : keys.quatValues.size();
        const auto count = static_cast<uint32_t>(std::min(keys.timestamps.size(), values));
        const auto times = static_cast<uint32_t>(out.size());
        out.insert(out.end(), keys.timestamps.begin(), keys.timestamps.begin() + count);
        const auto valuesAt = static_cast<uint32_t>(out.size());
        for (uint32_t k = 0; k < count; ++k) {
            if (kind == TrackKind::Vec3) {
                glm::vec3 v = keys.vec3Values[k];
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) v = fallback;
                out.insert(out.end(), {bits(v.x), bits(v.y), bits(v.z), 0u});
            } else {
                glm::quat q = keys.quatValues[k];
                const float lengthSquared = glm::dot(q, q);
                q = std::isfinite(lengthSquared) && lengthSquared >= 0.000001f ? glm::normalize(q)
                                                                               : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                out.insert(out.end(), {bits(q.x), bits(q.y), bits(q.z), bits(q.w)});
            }
        }
        const size_t entry = record + 3 + size_t(3) * s;
        out[entry] = count;
        out[entry + 1] = times;
        out[entry + 2] = valuesAt;
    }
    return record;
}

} // namespace

PackedSkeleton packSkeleton(const pipeline::M2Model& model, uint32_t boneLimit) {
    PackedSkeleton packed;
    const auto boneCount = static_cast<uint32_t>(std::min<size_t>(model.bones.size(), boneLimit));
    packed.boneCount = boneCount;
    auto& out = packed.words;
    out.resize(5 + size_t(kBoneWords) * boneCount, 0u);
    out[0] = boneCount;
    out[2] = 5;

    // Arm and depth from the parents, which come first; a parent listed after
    // its child (none the client ships) is not one.
    std::vector<int32_t> arm(boneCount, -1);
    std::vector<uint32_t> depth(boneCount, 0);
    bool misordered = false;
    for (uint32_t i = 0; i < boneCount; ++i) {
        const auto& bone = model.bones[i];
        int32_t parent = bone.parentBone;
        if (parent >= 0 && static_cast<uint32_t>(parent) >= i) {
            misordered = true;
            parent = -1;
        }
        arm[i] = bone.keyBoneId == kKeyBoneShoulderL ? 0
               : bone.keyBoneId == kKeyBoneShoulderR ? 1
               : parent >= 0 ? arm[parent] : -1;
        depth[i] = parent >= 0 ? depth[parent] + 1 : 0;
        packed.maxDepth = std::max(packed.maxDepth, depth[i]);
        const size_t record = 5 + size_t(kBoneWords) * i;
        out[record + 0] = bits(bone.pivot.x);
        out[record + 1] = bits(bone.pivot.y);
        out[record + 2] = bits(bone.pivot.z);
        out[record + 3] = static_cast<uint32_t>(parent);
        out[record + 4] = bone.flags;
        out[record + 5] = static_cast<uint32_t>(bone.keyBoneId);
        out[record + 6] = static_cast<uint32_t>(arm[i]);
        out[record + 7] = depth[i];
    }
    if (misordered)
        LOG_WARNING("Skeleton of '", model.name, "': a bone names a parent listed after it - posed as a root");
    out[1] = packed.maxDepth;
    for (uint32_t i = 0; i < boneCount; ++i) {
        const auto& bone = model.bones[i];
        const size_t record = 5 + size_t(kBoneWords) * i;
        const uint32_t translation = packTrack(bone.translation, TrackKind::Vec3, glm::vec3(0.0f), out);
        const uint32_t rotation = packTrack(bone.rotation, TrackKind::Quat, glm::vec3(0.0f), out);
        const uint32_t scale = packTrack(bone.scale, TrackKind::Vec3, glm::vec3(1.0f), out);
        out[record + 8] = translation;
        out[record + 9] = rotation;
        out[record + 10] = scale;
    }
    out[3] = static_cast<uint32_t>(model.globalSequenceDurations.size());
    out[4] = static_cast<uint32_t>(out.size());
    out.insert(out.end(), model.globalSequenceDurations.begin(), model.globalSequenceDurations.end());
    return packed;
}

SkeletonPool::~SkeletonPool() { destroy(); }

void SkeletonPool::destroy() {
    if (!context_ || pages_.empty()) return;
    // A frame in flight may still read them.
    context_->deferAfterAllFrameFences([allocator = context_->getAllocator(), pages = std::move(pages_)] {
        for (const auto& page : pages) {
            if (page) destroyBuffer(allocator, page->buffer);
        }
    });
    pages_.clear();
    ++version_;
}

void SkeletonPool::takeReturned() {
    std::vector<std::pair<std::shared_ptr<Page>, Skeleton>> skeletons;
    {
        std::lock_guard lock(returned_->mutex);
        skeletons.swap(returned_->skeletons);
    }
    for (const auto& [page, skeleton] : skeletons) page->words.free(skeleton.first, skeleton.words);
    // A page left empty goes, but for one shared one.
    bool spareKept = false;
    VmaAllocator allocator = context_->getAllocator();
    for (auto& page : pages_) {
        if (!page || page->words.used() != 0) continue;
        if (!spareKept && !page->fitted) {
            spareKept = true;
            continue;
        }
        destroyBuffer(allocator, page->buffer);
        page.reset();
        ++version_;
    }
    while (!pages_.empty() && !pages_.back()) pages_.pop_back();
}

SkeletonPool::Skeleton SkeletonPool::place(const pipeline::M2Model& model, uint32_t boneLimit, const char* name) {
    if (!context_ || model.bones.empty()) return {};
    takeReturned();
    const PackedSkeleton packed = packSkeleton(model, boneLimit);
    const auto words = static_cast<uint32_t>(packed.words.size());

    int32_t slot = -1;
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i] && !pages_[i]->fitted && pages_[i]->words.largestFree() >= words) {
            slot = static_cast<int32_t>(i);
            break;
        }
    }
    if (slot < 0) {
        const bool fitted = words > kPageWords;
        auto empty = std::find(pages_.begin(), pages_.end(), nullptr);
        const auto at = static_cast<size_t>(empty - pages_.begin());
        if (at >= kMaxPages) {
            if (!fullSaid_) {
                LOG_WARNING("Skeleton pool: all ", kMaxPages, " pages in use - '", name,
                            "' and the models after it are drawn in their bind pose until a page is let go");
                fullSaid_ = true;
            }
            return {};
        }
        auto page = std::make_shared<Page>();
        const uint32_t room = fitted ? words : kPageWords;
        page->buffer = createBuffer(context_->getAllocator(), VkDeviceSize(room) * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY);
        if (!page->buffer.buffer) {
            LOG_WARNING("Skeleton pool: no memory for a page of ", room, " words - '", name,
                        "' is drawn in its bind pose");
            return {};
        }
        page->words = RangeAllocator(room);
        page->fitted = fitted;
        if (empty == pages_.end()) pages_.push_back(std::move(page));
        else *empty = std::move(page);
        slot = static_cast<int32_t>(at);
        fullSaid_ = false;
        ++version_;
    }
    Page& page = *pages_[static_cast<size_t>(slot)];
    const auto first = page.words.allocate(words);
    if (!first || !uploadIntoBuffer(*context_, packed.words.data(), VkDeviceSize(words) * sizeof(uint32_t),
                                    page.buffer.buffer, VkDeviceSize(*first) * sizeof(uint32_t))) {
        if (first) page.words.free(*first, words);
        LOG_WARNING("Skeleton pool: '", name, "' could not be copied to the GPU - it is drawn in its bind pose");
        return {};
    }
    return Skeleton{.page = slot, .first = *first, .words = words, .boneCount = packed.boneCount};
}

void SkeletonPool::release(const Skeleton& skeleton) {
    if (!skeleton.placed() || static_cast<size_t>(skeleton.page) >= pages_.size() || !pages_[skeleton.page]) return;
    context_->deferAfterAllFrameFences([returned = returned_, page = pages_[skeleton.page], skeleton] {
        std::lock_guard lock(returned->mutex);
        returned->skeletons.emplace_back(page, skeleton);
    });
}

std::vector<::VkBuffer> SkeletonPool::pageBuffers() const {
    std::vector<::VkBuffer> buffers(pages_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < pages_.size(); ++i)
        if (pages_[i]) buffers[i] = pages_[i]->buffer.buffer;
    return buffers;
}

} // namespace rendering
} // namespace wowee
