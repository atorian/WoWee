#include "rendering/texture_cache.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace wowee {
namespace rendering {

namespace detail {

struct TextureEntry {
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

TextureRef::TextureRef(const TextureRef& other) : entry_(other.entry_) {
    if (!entry_) return;
    if (entry_->cache) entry_->cache->addRef(entry_);
    else ++entry_->refs;  // outlived its cache: nobody else touches it
}

TextureRef::TextureRef(TextureRef&& other) noexcept : entry_(std::exchange(other.entry_, nullptr)) {}

TextureRef& TextureRef::operator=(const TextureRef& other) {
    if (this != &other) {
        TextureRef copy(other);
        std::swap(entry_, copy.entry_);
    }
    return *this;
}

TextureRef& TextureRef::operator=(TextureRef&& other) noexcept {
    if (this != &other) {
        reset();
        entry_ = std::exchange(other.entry_, nullptr);
    }
    return *this;
}

TextureRef::~TextureRef() { reset(); }

void TextureRef::reset() {
    if (TextureEntry* entry = std::exchange(entry_, nullptr)) TextureCache::release(entry);
}

VkTexture* TextureRef::get() const { return entry_ ? entry_->texture.get() : nullptr; }

VkTexture* TextureRef::normalMap() const {
    if (!entry_) return nullptr;
    const TextureEntry* normal = entry_->normalMap.load(std::memory_order_acquire);
    return normal ? normal->texture.get() : nullptr;
}

TextureInfo TextureRef::normalMapInfo() const {
    if (!entry_) return {};
    const TextureEntry* normal = entry_->normalMap.load(std::memory_order_acquire);
    return normal ? normal->info : TextureInfo{};
}

const TextureInfo& TextureRef::info() const {
    static const TextureInfo kNone{};
    return entry_ ? entry_->info : kNone;
}

uint64_t TextureRef::bytes() const { return entry_ ? entry_->bytes : 0; }

const TextureKey& TextureRef::key() const {
    static const TextureKey kNone{};
    return entry_ ? entry_->key : kNone;
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
        // A command buffer submitted a frame or two ago may still sample it.
        device_.retireAfterFrames([texture = std::move(entry->texture), bytes,
                                   retiring = retiringBytes_]() mutable {
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
    if (!texture.entry_ || !normalMap.entry_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    // The reference moves into the texture's entry rather than being counted
    // again.
    TextureEntry* normal = std::exchange(normalMap.entry_, nullptr);
    if (TextureEntry* old = texture.entry_->normalMap.exchange(normal, std::memory_order_acq_rel)) {
        releaseLocked(old);
    }
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

} // namespace rendering
} // namespace wowee
