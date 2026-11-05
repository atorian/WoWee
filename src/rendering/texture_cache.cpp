#include "rendering/texture_cache.hpp"

#include "core/logger.hpp"
#include "rendering/vk_texture.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace wowee {
namespace rendering {

namespace detail {

// A revision names an appearance, not a cache-wide generation. Giving every
// new image and every changed normal link its own value also prevents reuse
// of an entry's address from looking like an unchanged binding.
uint64_t nextAppearanceRevision() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

struct TextureEntry {
    std::atomic<uint64_t> revision{nextAppearanceRevision()};
    TextureCache* cache = nullptr;
    TextureKey key;
    bool named = false;
    std::shared_ptr<VkTexture> texture;
    uint64_t bytes = 0;
    TextureInfo info;
    /// References held. Changed under the cache's lock.
    uint32_t refs = 0;
    /// Links in the cache's idle list while refs is zero.
    TextureEntry* idlePrev = nullptr;
    TextureEntry* idleNext = nullptr;
    bool idle = false;
    /// The normal map made from this texture, a reference this entry holds.
    /// Atomic because a frame recording on a worker thread reads it while the
    /// main thread may attach one.
    std::atomic<TextureEntry*> normalMap{nullptr};
    /// Its place in the texture table, given when admitted and given back
    /// with the texture, after the frames that may sample it.
    TextureHandle tableHandle = kNoTextureHandle;
};

// Written once, after the upload fence. The fallback and the resident image
// stay alive together: an older frame may still hold the fallback's pointer.
struct TextureFuture {
    std::atomic<uint64_t> revision{nextAppearanceRevision()};
    TextureKey key;
    TextureRef resident;
    std::atomic<TextureEntry*> ready{nullptr};
    std::atomic<bool> failed{false};
};

} // namespace detail

using detail::TextureEntry;

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

uint64_t mib(uint64_t bytes) { return static_cast<uint64_t>(static_cast<double>(bytes) / kMiB); }

} // namespace

const char* textureFormName(TextureForm form) {
    switch (form) {
    case TextureForm::Stored: return "textures";
    case TextureForm::Decoded: return "decoded textures";
    case TextureForm::NormalHeight: return "normal maps";
    case TextureForm::Composite: return "character composites";
    case TextureForm::Owned: return "owned textures";
    case TextureForm::Interface: return "interface textures";
    case TextureForm::Count_: break;
    }
    return "?";
}

std::string normalizeTexturePath(std::string_view path) {
    std::string key(path.substr(0, path.find('\0')));
    for (char& c : key) {
        c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (key.rfind(".\\", 0) == 0) key.erase(0, 2);
    const size_t first = key.find_first_not_of('\\');
    key.erase(0, first == std::string::npos ? key.size() : first);
    return key;
}

// ---- TextureRef --------------------------------------------------------------

TextureRef::TextureRef(TextureEntry* entry) : entry_(entry) {}

TextureRef::TextureRef(const TextureRef& other) : entry_(other.entry_), future_(other.future_) {
    if (!entry_) return;
    if (entry_->cache) entry_->cache->addRef(entry_);
    else ++entry_->refs;  // outlived its cache: nobody else touches it
}

TextureRef::TextureRef(TextureRef&& other) noexcept
    : entry_(std::exchange(other.entry_, nullptr)), future_(std::move(other.future_)) {}

TextureRef& TextureRef::operator=(const TextureRef& other) {
    if (this != &other) {
        TextureRef copy(other);
        std::swap(entry_, copy.entry_);
        future_.swap(copy.future_);
    }
    return *this;
}

TextureRef& TextureRef::operator=(TextureRef&& other) noexcept {
    if (this != &other) {
        reset();
        entry_ = std::exchange(other.entry_, nullptr);
        future_ = std::move(other.future_);
    }
    return *this;
}

TextureRef::~TextureRef() { reset(); }

void TextureRef::reset() {
    if (TextureEntry* entry = std::exchange(entry_, nullptr)) TextureCache::release(entry);
    future_.reset();
}

TextureRef TextureRef::pending(const TextureKey& key, TextureRef fallback) {
    TextureRef ref = fallback.snapshot();
    ref.future_ = std::make_shared<detail::TextureFuture>();
    ref.future_->key = key;
    return ref;
}

TextureRef TextureRef::withFallback(TextureRef fallback) const {
    if (!future_) return *this;
    TextureRef ref = fallback.snapshot();
    ref.future_ = future_;
    return ref;
}

void TextureRef::complete(TextureRef resident) {
    if (!future_ || !loading()) return;
    if (!resident || resident.loading() || resident.failed()) {
        future_->failed.store(true, std::memory_order_release);
        future_->revision.store(detail::nextAppearanceRevision(), std::memory_order_release);
        return;
    }
    future_->resident = std::move(resident);
    future_->ready.store(future_->resident.resolved(), std::memory_order_release);
}

bool TextureRef::loading() const {
    return future_ && !future_->ready.load(std::memory_order_acquire) &&
           !future_->failed.load(std::memory_order_acquire);
}

bool TextureRef::failed() const {
    return future_ && future_->failed.load(std::memory_order_acquire);
}

uint64_t TextureRef::revision() const {
    if (future_) {
        if (const TextureEntry* ready = future_->ready.load(std::memory_order_acquire))
            return ready->revision.load(std::memory_order_acquire);
        return future_->revision.load(std::memory_order_acquire);
    }
    return entry_ ? entry_->revision.load(std::memory_order_acquire) : 0;
}

TextureEntry* TextureRef::resolved() const {
    if (!future_) return entry_;
    if (TextureEntry* ready = future_->ready.load(std::memory_order_acquire)) return ready;
    return entry_;
}

VkTexture* TextureRef::get() const {
    const TextureEntry* entry = resolved();
    return entry ? entry->texture.get() : nullptr;
}

TextureRef TextureRef::snapshot() const {
    TextureEntry* entry = resolved();
    if (!entry) return {};
    if (entry->cache) entry->cache->addRef(entry);
    else ++entry->refs;
    return TextureRef(entry);
}

VkTexture* TextureRef::normalMap() const {
    const TextureEntry* entry = resolved();
    if (!entry) return nullptr;
    const TextureEntry* normal = entry->normalMap.load(std::memory_order_acquire);
    return normal ? normal->texture.get() : nullptr;
}

TextureInfo TextureRef::normalMapInfo() const {
    const TextureEntry* entry = resolved();
    if (!entry) return {};
    const TextureEntry* normal = entry->normalMap.load(std::memory_order_acquire);
    return normal ? normal->info : TextureInfo{};
}

TextureHandle TextureRef::tableHandle() const {
    const TextureEntry* entry = resolved();
    return entry ? entry->tableHandle : kNoTextureHandle;
}

TextureHandle TextureRef::normalMapTableHandle() const {
    const TextureEntry* entry = resolved();
    if (!entry) return kNoTextureHandle;
    const TextureEntry* normal = entry->normalMap.load(std::memory_order_acquire);
    return normal ? normal->tableHandle : kNoTextureHandle;
}

const TextureInfo& TextureRef::info() const {
    static const TextureInfo kNone{};
    const TextureEntry* entry = resolved();
    return entry ? entry->info : kNone;
}

uint64_t TextureRef::bytes() const {
    const TextureEntry* entry = resolved();
    return entry ? entry->bytes : 0;
}

const TextureKey& TextureRef::key() const {
    static const TextureKey kNone{};
    return future_ ? future_->key : entry_ ? entry_->key : kNone;
}

// ---- TextureCache -------------------------------------------------------------

TextureCache::Config TextureCache::configFromEnvironment() {
    Config config;
    if (const char* env = std::getenv("WOWEE_TEXTURE_CACHE_MB"); env && *env) {
        char* end = nullptr;
        const unsigned long long mb = std::strtoull(env, &end, 10);
        if (end && *end == '\0' && mb > 0) {
            config.budgetBytes = mb * 1024ull * 1024ull;
        } else {
            LOG_WARNING("WOWEE_TEXTURE_CACHE_MB='", env, "' is not a number of megabytes; the texture "
                        "cache keeps its ", mib(config.budgetBytes), " MB budget");
        }
    }
    return config;
}

TextureCache::TextureCache(TextureDevice& device, Config config)
    : device_(device), config_(config), retiringBytes_(std::make_shared<std::atomic<uint64_t>>(0)) {
    LOG_INFO("Texture cache: ", mib(config_.budgetBytes), " MB budget, at most ",
             static_cast<int>(config_.maxShareOfHeapBudget * 100.0),
             "% of what the driver allows in video memory");
}

TextureCache::~TextureCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    // First the links of what nobody holds: a normal map kept only by its
    // texture's entry is not still referenced by anyone.
    for (bool released = true; released;) {
        released = false;
        for (auto& [key, entry] : named_) {
            if (entry->refs != 0) continue;
            if (TextureEntry* normal = entry->normalMap.exchange(nullptr)) {
                releaseLocked(normal);
                released = true;
            }
        }
    }
    size_t held = 0;
    std::string example;
    const auto sweep = [&](auto& map) {
        for (auto it = map.begin(); it != map.end();) {
            TextureEntry* entry = it->second.get();
            if (entry->refs == 0) {
                ++it;
                continue;
            }
            // Something still holds it and will release it later, after the
            // device it lived on may be gone: leave the entry (and the image)
            // to that reference rather than free memory it still points at.
            ++held;
            if (example.empty()) example = entry->key.path.empty() ? textureFormName(entry->key.form)
                                                                     : entry->key.path;
            entry->cache = nullptr;
            new std::shared_ptr<VkTexture>(std::move(entry->texture));  // leaked on purpose
            (void)it->second.release();
            it = map.erase(it);
        }
    };
    sweep(named_);
    sweep(owned_);
    if (held > 0) {
        LOG_ERROR("Texture cache destroyed with ", held, " texture(s) still referenced (", example,
                  " among them); their images are left unfreed rather than freed under a holder");
    }
    // What is left is unreferenced: retired like any eviction, which the
    // device's final flush runs.
    for (auto& [key, entry] : named_) retireLocked(entry.get());
    for (auto& [ptr, entry] : owned_) retireLocked(entry.get());
    named_.clear();
    owned_.clear();
}

uint64_t TextureCache::effectiveBudgetLocked() const {
    uint64_t budget = config_.budgetBytes;
    if (const auto heap = device_.textureHeapBudget(); heap && *heap > 0) {
        const auto share = static_cast<uint64_t>(static_cast<double>(*heap) * config_.maxShareOfHeapBudget);
        if (share < budget) {
            budget = share;
            if (lastHeapLimitedBudget_ != share) {
                // Said again only when the driver's figure moves by a tenth.
                const uint64_t last = lastHeapLimitedBudget_;
                if (last == 0 || share < last - last / 10 || share > last + last / 10) {
                    LOG_WARNING("Texture cache: the driver allows ", mib(*heap),
                                " MB of video memory, so textures get ", mib(share), " MB of the ",
                                mib(config_.budgetBytes), " MB asked for");
                    lastHeapLimitedBudget_ = share;
                }
            }
        }
    }
    return budget;
}

uint64_t TextureCache::budgetBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return effectiveBudgetLocked();
}

void TextureCache::unlinkIdleLocked(TextureEntry* entry) {
    if (!entry->idle) return;
    (entry->idlePrev ? entry->idlePrev->idleNext : idleHead_) = entry->idleNext;
    (entry->idleNext ? entry->idleNext->idlePrev : idleTail_) = entry->idlePrev;
    entry->idlePrev = entry->idleNext = nullptr;
    entry->idle = false;
    idleBytes_ -= entry->bytes;
}

void TextureCache::linkIdleLocked(TextureEntry* entry) {
    entry->idlePrev = idleTail_;
    entry->idleNext = nullptr;
    (idleTail_ ? idleTail_->idleNext : idleHead_) = entry;
    idleTail_ = entry;
    entry->idle = true;
    idleBytes_ += entry->bytes;
}

void TextureCache::retireLocked(TextureEntry* entry) {
    unlinkIdleLocked(entry);
    // Its normal map goes back with it, to be evicted in turn when room is
    // wanted - or found again by whoever loads the texture next.
    if (TextureEntry* normal = entry->normalMap.exchange(nullptr, std::memory_order_acq_rel)) {
        releaseLocked(normal);
    }
    auto& form = forms_[static_cast<size_t>(entry->key.form)];
    form.entries -= 1;
    form.bytes -= entry->bytes;
    bytes_ -= entry->bytes;
    if (entry->texture) {
        const uint64_t bytes = entry->bytes;
        retiringBytes_->fetch_add(bytes);
        // A command buffer submitted a frame or two ago may still sample it,
        // through its place in the table as through the texture.
        device_.retireAfterFrames([texture = std::move(entry->texture), bytes, retiring = retiringBytes_,
                                   device = &device_, handle = entry->tableHandle]() mutable {
            if (handle != kNoTextureHandle) device->removeFromTable(handle);
            texture.reset();
            retiring->fetch_sub(bytes);
        });
    }
}

bool TextureCache::makeRoomLocked(uint64_t bytes) {
    const uint64_t budget = effectiveBudgetLocked();
    while (bytes_ + bytes > budget && idleHead_) {
        TextureEntry* victim = idleHead_;
        retireLocked(victim);
        if (victim->named) named_.erase(victim->key);
        else owned_.erase(victim);
    }
    return bytes_ + bytes <= budget;
}

bool TextureCache::reserve(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    return makeRoomLocked(bytes);
}

TextureRef TextureCache::admitLocked(std::unique_ptr<TextureEntry> entry, bool named) {
    TextureEntry* raw = entry.get();
    raw->cache = this;
    raw->named = named;
    raw->refs = 1;
    if (raw->texture) raw->tableHandle = device_.addToTable(*raw->texture);
    auto& form = forms_[static_cast<size_t>(raw->key.form)];
    form.entries += 1;
    form.bytes += raw->bytes;
    bytes_ += raw->bytes;
    if (named) named_.emplace(raw->key, std::move(entry));
    else owned_.emplace(raw, std::move(entry));
    return TextureRef(raw);
}

TextureRef TextureCache::find(const TextureKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = named_.find(key);
    if (it == named_.end()) return {};
    TextureEntry* entry = it->second.get();
    if (entry->refs++ == 0) unlinkIdleLocked(entry);
    return TextureRef(entry);
}

TextureRef TextureCache::defer(const TextureKey& key, TextureRef fallback) {
    return TextureRef::pending(key, std::move(fallback));
}

void TextureCache::complete(TextureRef request, TextureRef resident) {
    request.complete(std::move(resident));
}

void TextureCache::invalidateNamed() {
    std::lock_guard lock(mutex_);
    for (auto& [key, entry] : named_) {
        if (entry->refs == 0) retireLocked(entry.get());
        else {
            entry->named = false;
            TextureEntry* raw = entry.get();
            owned_.emplace(raw, std::move(entry));
        }
    }
    named_.clear();
}

TextureRef TextureCache::insert(const TextureKey& key, std::shared_ptr<VkTexture> texture,
                                uint64_t bytes, TextureInfo info) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = named_.find(key); it != named_.end()) {
        // Two loads of one key raced, or a caller did not look first. The
        // first upload is the one in use; this one goes the way an eviction
        // does, since nothing has drawn with it yet but the upload may still be
        // in flight.
        if (texture) {
            retiringBytes_->fetch_add(bytes);
            device_.retireAfterFrames([texture = std::move(texture), bytes,
                                       retiring = retiringBytes_]() mutable {
                texture.reset();
                retiring->fetch_sub(bytes);
            });
        }
        TextureEntry* entry = it->second.get();
        if (entry->refs++ == 0) unlinkIdleLocked(entry);
        return TextureRef(entry);
    }
    if (!makeRoomLocked(bytes)) {
        ++refusals_;
        if (!refusing_) {
            refusing_ = true;
            LOG_WARNING("Texture cache full: ", mib(bytes_), " MB of ", mib(effectiveBudgetLocked()),
                        " MB held, all of it by what is drawn now - '", key.path, "' (",
                        textureFormName(key.form), ", ", mib(bytes),
                        " MB) and what follows it are drawn with their fallback until something "
                        "is released");
        }
        retiringBytes_->fetch_add(bytes);
        device_.retireAfterFrames([texture = std::move(texture), bytes, retiring = retiringBytes_]() mutable {
            texture.reset();
            retiring->fetch_sub(bytes);
        });
        return {};
    }
    if (refusing_) {
        refusing_ = false;
        LOG_WARNING("Texture cache: textures fit again (", mib(bytes_ + bytes), " MB of ",
                    mib(effectiveBudgetLocked()), " MB) after ", refusals_, " refusal(s) in all");
    }
    auto entry = std::make_unique<TextureEntry>();
    entry->key = key;
    entry->texture = std::move(texture);
    entry->bytes = bytes;
    entry->info = info;
    return admitLocked(std::move(entry), /*named=*/true);
}

TextureRef TextureCache::adopt(TextureForm form, std::shared_ptr<VkTexture> texture, uint64_t bytes,
                               TextureInfo info) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)makeRoomLocked(bytes);
    auto entry = std::make_unique<TextureEntry>();
    entry->key.form = form;
    entry->texture = std::move(texture);
    entry->bytes = bytes;
    entry->info = info;
    return admitLocked(std::move(entry), /*named=*/false);
}

void TextureCache::addRef(TextureEntry* entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry->refs++ == 0) unlinkIdleLocked(entry);
}

void TextureCache::release(TextureEntry* entry) {
    if (TextureCache* cache = entry->cache) {
        std::lock_guard<std::mutex> lock(cache->mutex_);
        cache->releaseLocked(entry);
        return;
    }
    // Outlived its cache (logged when the cache went): the entry is the last
    // reference's to free; its image, and any normal map's, are not.
    if (--entry->refs > 0) return;
    if (TextureEntry* normal = entry->normalMap.exchange(nullptr)) release(normal);
    delete entry;
}

void TextureCache::releaseLocked(TextureEntry* entry) {
    if (--entry->refs > 0) return;
    if (entry->named) {
        linkIdleLocked(entry);
        return;
    }
    // Nobody can ask for it again.
    retireLocked(entry);
    owned_.erase(entry);
}

void TextureCache::setNormalMap(const TextureRef& texture, TextureRef normalMap) {
    if (texture.loading() || texture.failed() || normalMap.loading() || normalMap.failed()) return;
    TextureEntry* entry = texture.resolved();
    TextureEntry* normal = normalMap.resolved();
    if (!entry || !normal) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry->normalMap.load(std::memory_order_relaxed) == normal) return;
    if (normal->refs++ == 0) unlinkIdleLocked(normal);
    if (TextureEntry* old = entry->normalMap.exchange(normal, std::memory_order_acq_rel)) {
        releaseLocked(old);
    }
    entry->revision.store(detail::nextAppearanceRevision(), std::memory_order_release);
}

TextureCacheStats TextureCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TextureCacheStats out;
    out.forms = forms_;
    out.entries = named_.size() + owned_.size();
    out.bytes = bytes_;
    out.idleBytes = idleBytes_;
    out.retiringBytes = retiringBytes_->load();
    out.budgetBytes = effectiveBudgetLocked();
    out.refusals = refusals_;
    return out;
}

std::vector<TextureCache::EntryReport> TextureCache::largestEntries(size_t count) const {
    std::vector<EntryReport> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.reserve(named_.size() + owned_.size());
        const auto report = [&](const TextureEntry& e) {
            out.push_back({.key = e.key,
                           .bytes = e.bytes,
                           .width = e.texture ? e.texture->getWidth() : 0,
                           .height = e.texture ? e.texture->getHeight() : 0,
                           .refs = e.refs});
        };
        for (const auto& [key, entry] : named_) report(*entry);
        for (const auto& [ptr, entry] : owned_) report(*entry);
    }
    const size_t keep = std::min(count, out.size());
    std::partial_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(keep), out.end(),
                      [](const EntryReport& a, const EntryReport& b) { return a.bytes > b.bytes; });
    out.resize(keep);
    return out;
}

} // namespace rendering
} // namespace wowee
