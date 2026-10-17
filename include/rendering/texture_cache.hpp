#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wowee {
namespace rendering {

class VkTexture;
class TextureCache;

/// What a texture in the cache is: a file as it is stored, or something made
/// from one. Part of the key, because the same path in two forms is two
/// different images on the GPU.
enum class TextureForm : uint8_t {
    /// The file uploaded as the loader keeps it: DXT blocks with the file's
    /// own mips where it is compressed. What the world's models and the
    /// ground sample.
    Stored,
    /// The file decoded to RGBA8 and keyed the way the character renderer
    /// keys it: a skin or a piece of armour.
    Decoded,
    /// A normal and height map derived from the file's colours. The key's
    /// variant is the strength it was derived with.
    NormalHeight,
    /// A character's skin composed in memory from layers. The key's path names
    /// the layers.
    Composite,
    /// Made for one owner and found by nobody else - a terrain chunk's alpha
    /// map, a renderer's fallback. Counted against the budget like the rest;
    /// gone when its last reference is.
    Owned,
    Count_,
};

inline constexpr size_t kTextureFormCount = static_cast<size_t>(TextureForm::Count_);

[[nodiscard]] const char* textureFormName(TextureForm form);

/// A texture path the way the cache keys it: backslashes, lower case, no
/// leading ".\\" or "\\", nothing after an embedded NUL.
[[nodiscard]] std::string normalizeTexturePath(std::string_view path);

struct TextureKey {
    /// Normalized (normalizeTexturePath).
    std::string path;
    TextureForm form = TextureForm::Stored;
    /// What else tells two images of one path and form apart: the strength a
    /// normal map was derived with.
    uint8_t variant = 0;

    bool operator==(const TextureKey&) const = default;
};

struct TextureKeyHash {
    size_t operator()(const TextureKey& key) const {
        return std::hash<std::string>{}(key.path) ^ (static_cast<size_t>(key.form) << 1) ^
               (static_cast<size_t>(key.variant) << 8);
    }
};

/// What the renderers read about a texture besides its pixels, kept with it
/// so it lives and dies with the image rather than in a side table keyed by a
/// pointer that can be reused.
struct TextureInfo {
    /// Some texel is not opaque.
    bool hasAlpha = false;
    /// The alpha draws a silhouette (leaves, a lattice) rather than being a
    /// leftover of the atlas.
    bool alphaIsSilhouette = false;
    /// For a normal and height map: the variance of its heights.
    float heightVariance = 0.0f;
};

namespace detail {
struct TextureEntry;
}

/// A counted reference to one texture in the cache. While any reference to an
/// entry exists the entry stays resident; with none it may be evicted, and an
/// entry nobody can look up again (TextureForm::Owned) is retired at once.
///
/// Draw structures may keep the raw VkTexture* that get() returns, so long as
/// whatever owns them also keeps the reference: a model holds the references
/// for every texture its batches point at.
///
/// Must not outlive the cache it came from, which lives as long as the device.
class TextureRef {
public:
    TextureRef() = default;
    TextureRef(const TextureRef& other);
    TextureRef(TextureRef&& other) noexcept;
    TextureRef& operator=(const TextureRef& other);
    TextureRef& operator=(TextureRef&& other) noexcept;
    ~TextureRef();

    [[nodiscard]] VkTexture* get() const;
    VkTexture* operator->() const { return get(); }
    explicit operator bool() const { return entry_ != nullptr; }
    [[nodiscard]] const TextureInfo& info() const;
    /// What it is cached under; an empty path for one only its owner holds.
    [[nodiscard]] const TextureKey& key() const;
    /// What the entry counts as costing on the GPU.
    [[nodiscard]] uint64_t bytes() const;

    /// The normal map made from this texture (TextureCache::setNormalMap),
    /// null until there is one. Kept by the texture's entry, so it lives as
    /// long as the texture and is released with it. Safe to read while the
    /// main thread attaches one.
    [[nodiscard]] VkTexture* normalMap() const;
    /// Its TextureInfo (heightVariance); empty without one.
    [[nodiscard]] TextureInfo normalMapInfo() const;

    void reset();

    friend bool operator==(const TextureRef& a, const TextureRef& b) { return a.entry_ == b.entry_; }

private:
    friend class TextureCache;
    explicit TextureRef(detail::TextureEntry* entry);  // takes a reference already counted
    detail::TextureEntry* entry_ = nullptr;
};

/// What the cache needs of the device: VkContext in the client, a fake in a
/// test.
class TextureDevice {
public:
    virtual ~TextureDevice() = default;
    /// Run `release` once no frame in flight can still read what it frees.
    virtual void retireAfterFrames(std::function<void()> release) = 0;
    /// What the driver lets the client hold in the heap textures live in.
    [[nodiscard]] virtual std::optional<uint64_t> textureHeapBudget() const = 0;
};

struct TextureCacheStats {
    struct Form {
        size_t entries = 0;
        uint64_t bytes = 0;
    };
    std::array<Form, kTextureFormCount> forms{};
    size_t entries = 0;
    uint64_t bytes = 0;
    /// Of `bytes`, what nothing references and eviction may take.
    uint64_t idleBytes = 0;
    /// Evicted or released, still on the GPU until the frames that may read
    /// it have finished.
    uint64_t retiringBytes = 0;
    /// The budget in force: the configured one, or the share of the driver's
    /// heap budget when that is smaller.
    uint64_t budgetBytes = 0;
    /// Textures refused because everything resident was in use.
    uint64_t refusals = 0;

    [[nodiscard]] const Form& form(TextureForm f) const { return forms[static_cast<size_t>(f)]; }
};

/// The one owner of the renderers' textures on the GPU, under one budget.
///
/// One entry per key, whichever renderer asks; the textures made from a file
/// (its normal map, a character's composite) are entries of their own and
/// counted the same way. Renderers hold references (TextureRef). When an
/// insert would go over the budget, entries nothing references are evicted,
/// least recently used first, and retired through the device once the frames
/// that may still read them are done - never by waiting for the device.
///
/// When everything resident is in use and the new texture still does not fit,
/// it is refused: the caller draws its fallback, and the refusal is said once
/// until something fits again.
///
/// Thread-safe; in practice used from the main thread.
class TextureCache {
public:
    struct Config {
        /// The most it may hold.
        uint64_t budgetBytes = 4096ull * 1024 * 1024;
        /// And no more than this share of what the driver allows the process
        /// in the heap - the targets, the geometry and the desktop need the rest.
        double maxShareOfHeapBudget = 0.5;
    };

    /// The configured budget: WOWEE_TEXTURE_CACHE_MB, or 4096 MB.
    [[nodiscard]] static Config configFromEnvironment();

    TextureCache(TextureDevice& device, Config config);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    /// The entry for `key`, if resident. A hit makes it the most recently used.
    [[nodiscard]] TextureRef find(const TextureKey& key);

    /// Whether `bytes` more would fit, evicting what nothing references to
    /// make room. Lets a caller try a smaller upload before giving up.
    [[nodiscard]] bool reserve(uint64_t bytes);

    /// Take ownership of `texture`, `bytes` large, under `key`. Evicts to make
    /// room; refused (empty reference, logged once) when it cannot. If `key`
    /// is already resident the existing entry is returned and `texture` is
    /// retired: the first upload of a key is the one everybody shares.
    [[nodiscard]] TextureRef insert(const TextureKey& key, std::shared_ptr<VkTexture> texture,
                                    uint64_t bytes, TextureInfo info = {});

    /// Take ownership of a texture nobody else will look up (TextureForm::Owned
    /// or any form for a one-off). Counted against the budget, and evicts to
    /// make room, but is never refused: what asks for one has nothing else to
    /// draw with. Retired when its last reference goes.
    [[nodiscard]] TextureRef adopt(TextureForm form, std::shared_ptr<VkTexture> texture,
                                   uint64_t bytes, TextureInfo info = {});

    /// Make `normalMap` the normal map of `texture`: the texture's entry
    /// holds it from now on and lets it go when it is evicted, so the one is
    /// resident for as long as the other. Replaces one attached before.
    void setNormalMap(const TextureRef& texture, TextureRef normalMap);

    [[nodiscard]] TextureCacheStats stats() const;
    [[nodiscard]] uint64_t budgetBytes() const;

private:
    friend class TextureRef;
    void addRef(detail::TextureEntry* entry);
    /// A reference given back, by whichever cache the entry belongs to - or
    /// by none, for one that outlived its cache.
    static void release(detail::TextureEntry* entry);
    void releaseLocked(detail::TextureEntry* entry);

    /// Under the lock.
    [[nodiscard]] uint64_t effectiveBudgetLocked() const;
    [[nodiscard]] bool makeRoomLocked(uint64_t bytes);
    void retireLocked(detail::TextureEntry* entry);
    void unlinkIdleLocked(detail::TextureEntry* entry);
    void linkIdleLocked(detail::TextureEntry* entry);
    TextureRef admitLocked(std::unique_ptr<detail::TextureEntry> entry, bool named);

    TextureDevice& device_;
    Config config_;
    mutable std::mutex mutex_;
    std::unordered_map<TextureKey, std::unique_ptr<detail::TextureEntry>, TextureKeyHash> named_;
    std::unordered_map<detail::TextureEntry*, std::unique_ptr<detail::TextureEntry>> owned_;
    /// Entries nothing references, least recently used at the head.
    detail::TextureEntry* idleHead_ = nullptr;
    detail::TextureEntry* idleTail_ = nullptr;
    std::array<TextureCacheStats::Form, kTextureFormCount> forms_{};
    uint64_t bytes_ = 0;
    uint64_t idleBytes_ = 0;
    /// Shared with the retirements still queued on the device, which may run
    /// after the cache is gone.
    std::shared_ptr<std::atomic<uint64_t>> retiringBytes_;
    uint64_t refusals_ = 0;
    /// Said once per stretch of refusals, and the heap budget's limit once per
    /// change.
    bool refusing_ = false;
    mutable uint64_t lastHeapLimitedBudget_ = 0;
};

} // namespace rendering
} // namespace wowee
