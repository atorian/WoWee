#include "rendering/skeleton_pool.hpp"

#include "core/logger.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_particle_emission.hpp"
#include "rendering/retired_pages.hpp"
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

enum class TrackKind { Vec3, Quat, Float };

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
        const size_t values = kind == TrackKind::Vec3 ? keys.vec3Values.size()
                            : kind == TrackKind::Quat ? keys.quatValues.size() : keys.floatValues.size();
        const auto count = static_cast<uint32_t>(std::min(keys.timestamps.size(), values));
        const auto times = static_cast<uint32_t>(out.size());
        out.insert(out.end(), keys.timestamps.begin(), keys.timestamps.begin() + count);
        const auto valuesAt = static_cast<uint32_t>(out.size());
        for (uint32_t k = 0; k < count; ++k) {
            if (kind == TrackKind::Float) {
                // As m2_track::sampleFloat reads them: not cleaned.
                out.insert(out.end(), {bits(keys.floatValues[k]), 0u, 0u, 0u});
            } else if (kind == TrackKind::Vec3) {
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

enum class FBlockKind { Float, Vec2, Vec3 };

/// An FBlock appended to `out`: its value count, its time count, the times,
/// then four words a value. Its first word, or kNone for one with no values.
uint32_t packFBlock(const pipeline::M2FBlock& block, FBlockKind kind, std::vector<uint32_t>& out) {
    const size_t values = kind == FBlockKind::Float ? block.floatValues.size()
                        : kind == FBlockKind::Vec2  ? block.vec2Values.size()
                                                    : block.vec3Values.size();
    if (values == 0) return kNone;
    const auto record = static_cast<uint32_t>(out.size());
    out.push_back(static_cast<uint32_t>(values));
    out.push_back(static_cast<uint32_t>(block.timestamps.size()));
    for (const float t : block.timestamps) out.push_back(bits(t));
    for (size_t k = 0; k < values; ++k) {
        glm::vec4 v(0.0f);
        if (kind == FBlockKind::Float) v.x = block.floatValues[k];
        else if (kind == FBlockKind::Vec2) v = glm::vec4(block.vec2Values[k], 0.0f, 0.0f);
        else v = glm::vec4(block.vec3Values[k], 0.0f);
        out.insert(out.end(), {bits(v.x), bits(v.y), bits(v.z), bits(v.w)});
    }
    return record;
}

/// The most a float track's keys reach, or `none` above them.
float trackMaximum(const pipeline::M2AnimationTrack& track, float none) {
    float most = none;
    for (const auto& keys : track.sequences)
        for (const float v : keys.floatValues)
            if (std::isfinite(v)) most = std::max(most, v);
    return most;
}

} // namespace

PackedEmitters packEmitters(const pipeline::M2Model& model) {
    PackedEmitters packed;
    const auto count = static_cast<uint32_t>(model.particleEmitters.size());
    auto& out = packed.words;
    out.resize(kEmitterHeaderWords + size_t(kEmitterWords) * count, 0u);
    out[0] = count;
    out[2] = kEmitterHeaderWords;
    for (uint32_t e = 0; e < count; ++e) {
        const auto& em = model.particleEmitters[e];
        // The most it may ever hold (m2EmitterPool at its highest rate and
        // longest life): its particles' room, which the GPU grows its pool in.
        const uint32_t most = m2EmitterPool(trackMaximum(em.emissionRate, 0.0f), em.emissionRateVary,
                                            trackMaximum(em.lifespan, 0.0f), em.lifespanVary);
        uint32_t capacity = most;
        if (capacity > kMaxEmitterParticles) {
            LOG_WARNING("Particle emitter ", e, " of '", model.name, "' may hold ", capacity,
                        " particles; it is given room for ", kMaxEmitterParticles, " and emits no more");
            capacity = kMaxEmitterParticles;
        }
        packed.capacities.push_back(capacity);
        if (em.emitterType == 3)
            LOG_WARNING("Spline particle emitter ", e, " of '", model.name,
                        "' emits from its origin; spline spawning is not implemented");
        const size_t r = kEmitterHeaderWords + size_t(kEmitterWords) * e;
        out[r + 0] = em.flags;
        out[r + 1] = em.emitterType;
        out[r + 2] = em.bone;
        out[r + 3] = uint32_t(std::max<uint16_t>(em.textureCols, 1)) | (uint32_t(std::max<uint16_t>(em.textureRows, 1)) << 16u);
        out[r + 4] = bits(em.position.x);
        out[r + 5] = bits(em.position.y);
        out[r + 6] = bits(em.position.z);
        out[r + 7] = bits(em.drag);
        out[r + 8] = bits(em.tailLength);
        out[r + 9] = (em.drawsHead ? 1u : 0u) | (em.drawsTail ? 2u : 0u) | (em.enabled ? 4u : 0u);
        out[r + 10] = bits(em.scaleVary.x);
        out[r + 11] = bits(em.scaleVary.y);
        out[r + 12] = bits(em.twinkleSpeed);
        out[r + 13] = bits(em.twinklePercent);
        out[r + 14] = bits(em.twinkleScaleMin);
        out[r + 15] = bits(em.twinkleScaleMax);
        out[r + 16] = bits(em.baseSpin);
        out[r + 17] = bits(em.baseSpinVary);
        out[r + 18] = bits(em.spin);
        out[r + 19] = bits(em.spinVary);
        out[r + 20] = bits(em.wind.x);
        out[r + 21] = bits(em.wind.y);
        out[r + 22] = bits(em.wind.z);
        out[r + 23] = bits(em.windTime);
        out[r + 24] = bits(em.lifespanVary);
        out[r + 25] = bits(em.emissionRateVary);
        out[r + 26] = capacity;
    }
    for (uint32_t e = 0; e < count; ++e) {
        const auto& em = model.particleEmitters[e];
        const size_t r = kEmitterHeaderWords + size_t(kEmitterWords) * e;
        const pipeline::M2AnimationTrack* tracks[] = {&em.emissionSpeed, &em.speedVariation, &em.verticalRange,
                                                      &em.horizontalRange, &em.gravity, &em.lifespan,
                                                      &em.emissionRate, &em.emissionAreaLength,
                                                      &em.emissionAreaWidth, &em.zSource};
        for (size_t t = 0; t < std::size(tracks); ++t) {
            const uint32_t at = packTrack(*tracks[t], TrackKind::Float, glm::vec3(0.0f), out);
            out[r + kEmitterTracks + t] = at;
        }
        const uint32_t color = packFBlock(em.particleColor, FBlockKind::Vec3, out);
        const uint32_t alpha = packFBlock(em.particleAlpha, FBlockKind::Float, out);
        const uint32_t scale = packFBlock(em.particleScale, FBlockKind::Vec2, out);
        const uint32_t headCell = packFBlock(em.headCellTrack, FBlockKind::Float, out);
        const uint32_t tailCell = packFBlock(em.tailCellTrack, FBlockKind::Float, out);
        out[r + kEmitterFBlocks + 0] = color;
        out[r + kEmitterFBlocks + 1] = alpha;
        out[r + kEmitterFBlocks + 2] = scale;
        out[r + kEmitterFBlocks + 3] = headCell;
        out[r + kEmitterFBlocks + 4] = tailCell;
    }
    // The ribbons: their records, then their tracks.
    const auto ribbons = static_cast<uint32_t>(model.ribbonEmitters.size());
    const auto ribbonRecords = static_cast<uint32_t>(out.size());
    out[1] = ribbons;
    out[5] = ribbonRecords;
    out.resize(out.size() + size_t(kRibbonWords) * ribbons, 0u);
    for (uint32_t e = 0; e < ribbons; ++e) {
        const auto& em = model.ribbonEmitters[e];
        // As many edges as it lays over an edge's life, and the two it is
        // between (CRibbonEmitter).
        const float edgesPerSecond = std::max(std::ceil(em.edgesPerSecond), 1.0f);
        const float lifetime = std::max(em.edgeLifetime, 0.25f);
        const auto capacity = static_cast<uint32_t>(std::ceil(lifetime * edgesPerSecond)) + 2;
        packed.ribbonCapacities.push_back(capacity);
        const size_t r = ribbonRecords + size_t(kRibbonWords) * e;
        out[r + 0] = em.bone;
        out[r + 1] = bits(em.position.x);
        out[r + 2] = bits(em.position.y);
        out[r + 3] = bits(em.position.z);
        out[r + 4] = bits(edgesPerSecond);
        out[r + 5] = bits(lifetime);
        out[r + 6] = bits(em.gravity);
        out[r + 7] = capacity;
    }
    for (uint32_t e = 0; e < ribbons; ++e) {
        const auto& em = model.ribbonEmitters[e];
        const size_t r = ribbonRecords + size_t(kRibbonWords) * e;
        const uint32_t color = packTrack(em.colorTrack, TrackKind::Vec3, glm::vec3(1.0f), out);
        const uint32_t alpha = packTrack(em.alphaTrack, TrackKind::Float, glm::vec3(0.0f), out);
        const uint32_t above = packTrack(em.heightAboveTrack, TrackKind::Float, glm::vec3(0.0f), out);
        const uint32_t below = packTrack(em.heightBelowTrack, TrackKind::Float, glm::vec3(0.0f), out);
        const uint32_t visibility = packTrack(em.visibilityTrack, TrackKind::Float, glm::vec3(0.0f), out);
        out[r + kRibbonTracks + 0] = color;
        out[r + kRibbonTracks + 1] = alpha;
        out[r + kRibbonTracks + 2] = above;
        out[r + kRibbonTracks + 3] = below;
        out[r + kRibbonTracks + 4] = visibility;
    }
    out[3] = static_cast<uint32_t>(model.globalSequenceDurations.size());
    out[4] = static_cast<uint32_t>(out.size());
    out.insert(out.end(), model.globalSequenceDurations.begin(), model.globalSequenceDurations.end());
    return packed;
}

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

std::vector<uint32_t> packTextureAnimations(const pipeline::M2Model& model) {
    const auto count = static_cast<uint32_t>(model.textureTransforms.size());
    std::vector<uint32_t> out(5 + size_t(3) * count, 0u);
    out[0] = count;
    out[2] = 5;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& transform = model.textureTransforms[i];
        const size_t record = 5 + size_t(3) * i;
        const uint32_t translation = packTrack(transform.translation, TrackKind::Vec3, glm::vec3(0.0f), out);
        const uint32_t rotation = packTrack(transform.rotation, TrackKind::Quat, glm::vec3(0.0f), out);
        const uint32_t scale = packTrack(transform.scale, TrackKind::Vec3, glm::vec3(1.0f), out);
        out[record + 0] = translation;
        out[record + 1] = rotation;
        out[record + 2] = scale;
    }
    out[3] = static_cast<uint32_t>(model.globalSequenceDurations.size());
    out[4] = static_cast<uint32_t>(out.size());
    out.insert(out.end(), model.globalSequenceDurations.begin(), model.globalSequenceDurations.end());
    return out;
}

SkeletonPool::~SkeletonPool() { destroy(); }

void SkeletonPool::destroy() {
    if (!context_ || pages_.empty()) return;
    // A frame in flight may still read them.
    retirePages(*context_, pages_);
    held_.clear();
    contentAt_.clear();
    ++version_;
}

void SkeletonPool::takeReturned() {
    std::vector<Returned::Blob> blobs;
    {
        std::lock_guard lock(returned_->mutex);
        blobs.swap(returned_->blobs);
    }
    for (const auto& blob : blobs) blob.page->words.free(blob.first, blob.words);
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
    const PackedSkeleton packed = packSkeleton(model, boneLimit);
    const auto at = placeWords(packed.words, name, "drawn in its bind pose");
    if (!at) return {};
    return Skeleton{.page = at->first, .first = at->second, .words = static_cast<uint32_t>(packed.words.size()),
                    .boneCount = packed.boneCount};
}

SkeletonPool::Emitters SkeletonPool::placeEmitters(const pipeline::M2Model& model, const char* name) {
    if (!context_ || (model.particleEmitters.empty() && model.ribbonEmitters.empty())) return {};
    const PackedEmitters packed = packEmitters(model);
    const auto at = placeWords(packed.words, name, "drawn without its particles and ribbons");
    if (!at) return {};
    return Emitters{.page = at->first, .first = at->second, .words = static_cast<uint32_t>(packed.words.size()),
                    .capacities = packed.capacities, .ribbonCapacities = packed.ribbonCapacities};
}

SkeletonPool::Content SkeletonPool::contentOf(const std::vector<uint32_t>& blob) {
    // FNV-1a over the words, and a multiply-rotate of them from another
    // seed: data no one chose to collide, told apart by 128 bits.
    Content c{.a = 0xcbf29ce484222325ull, .b = 0x9e3779b97f4a7c15ull, .words = static_cast<uint32_t>(blob.size())};
    for (const uint32_t w : blob) {
        c.a = (c.a ^ w) * 0x100000001b3ull;
        c.b = std::rotl(c.b + w * 0xc2b2ae3d27d4eb4full, 31) * 0x9e3779b185ebca87ull;
    }
    return c;
}

std::optional<std::pair<int32_t, uint32_t>> SkeletonPool::placeWords(const std::vector<uint32_t>& blob,
                                                                    const char* name, const char* without) {
    takeReturned();
    const Content content = contentOf(blob);
    if (auto it = held_.find(content); it != held_.end()) {
        ++it->second.users;
        return std::make_pair(it->second.page, it->second.first);
    }
    const auto at = copyWords(blob, name, without);
    if (!at) return std::nullopt;
    held_.emplace(content, Held{.page = at->first, .first = at->second, .users = 1});
    contentAt_.emplace((uint64_t(uint32_t(at->first)) << 32) | at->second, content);
    return at;
}

std::optional<std::pair<int32_t, uint32_t>> SkeletonPool::copyWords(const std::vector<uint32_t>& blob,
                                                                   const char* name, const char* without) {
    const auto words = static_cast<uint32_t>(blob.size());
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
                LOG_WARNING("Skeleton pool: all ", kMaxPages, " pages in use - '", name, "' is ", without,
                            ", and so are the models after it until a page is let go");
                fullSaid_ = true;
            }
            return std::nullopt;
        }
        auto page = std::make_shared<Page>();
        const uint32_t room = fitted ? words : kPageWords;
        page->buffer = createBuffer(context_->getAllocator(), VkDeviceSize(room) * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY);
        if (!page->buffer.buffer) {
            LOG_WARNING("Skeleton pool: no memory for a page of ", room, " words - '", name, "' is ", without);
            return std::nullopt;
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
    if (!first || !uploadIntoBuffer(*context_, blob.data(), VkDeviceSize(words) * sizeof(uint32_t),
                                    page.buffer.buffer, VkDeviceSize(*first) * sizeof(uint32_t))) {
        if (first) page.words.free(*first, words);
        LOG_WARNING("Skeleton pool: '", name, "' could not be copied to the GPU - it is ", without);
        return std::nullopt;
    }
    return std::make_pair(slot, *first);
}

SkeletonPool::Skeleton SkeletonPool::placeTextureAnimations(const pipeline::M2Model& model, const char* name) {
    if (!context_ || model.textureTransforms.empty()) return {};
    const std::vector<uint32_t> words = packTextureAnimations(model);
    const auto at = placeWords(words, name, "drawn with its textures standing still");
    if (!at) return {};
    return Skeleton{.page = at->first, .first = at->second, .words = static_cast<uint32_t>(words.size()),
                    .boneCount = static_cast<uint32_t>(model.textureTransforms.size())};
}

SkeletonPool::Skeleton SkeletonPool::placeBlob(const std::vector<uint32_t>& words, const char* name,
                                               const char* without) {
    if (!context_ || words.empty()) return {};
    const auto at = placeWords(words, name, without);
    if (!at) return {};
    return Skeleton{.page = at->first, .first = at->second, .words = static_cast<uint32_t>(words.size())};
}

void SkeletonPool::release(const Skeleton& skeleton) {
    if (skeleton.placed()) releaseWords(skeleton.page, skeleton.first, skeleton.words);
}

void SkeletonPool::release(const Emitters& emitters) {
    if (emitters.placed()) releaseWords(emitters.page, emitters.first, emitters.words);
}

void SkeletonPool::releaseWords(int32_t pageIndex, uint32_t first, uint32_t words) {
    // Every placed blob is held under its content; a release of one that is
    // not is let go twice or never placed, and its words may be another
    // model's now - they stay where they are.
    const uint64_t where = (uint64_t(uint32_t(pageIndex)) << 32) | first;
    const auto at = contentAt_.find(where);
    if (static_cast<size_t>(pageIndex) >= pages_.size() || !pages_[pageIndex] || at == contentAt_.end()) {
        LOG_ERROR("Skeleton pool: page ", pageIndex, " word ", first, " (", words,
                  " words) let go but not held - left in place");
        return;
    }
    Held& held = held_.at(at->second);
    if (--held.users > 0) return;
    held_.erase(at->second);
    contentAt_.erase(at);
    context_->deferAfterAllFrameFences([returned = returned_, page = pages_[pageIndex], first, words] {
        std::lock_guard lock(returned->mutex);
        returned->blobs.push_back({.page = page, .first = first, .words = words});
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
