#include "pipeline/asset_manager.hpp"
#include "pipeline/base_fallback.hpp"
#include "pipeline/dds_loader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_set>

#include "stb_image.h"

namespace wowee {
namespace pipeline {

namespace {
size_t parseEnvSizeMB(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return 0;
    }
    char* end = nullptr;
    unsigned long long mb = std::strtoull(v, &end, 10);
    if (end == v || mb == 0) {
        return 0;
    }
    if (mb > (std::numeric_limits<size_t>::max() / (1024ull * 1024ull))) {
        return 0;
    }
    return static_cast<size_t>(mb);
}

size_t parseEnvCount(const char* name, size_t defValue) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return defValue;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v || n == 0) {
        return defValue;
    }
    return static_cast<size_t>(n);
}
} // namespace

AssetManager::AssetManager() = default;
AssetManager::~AssetManager() {
    shutdown();
}

bool AssetManager::initialize(const std::string& dataPath_) {
    if (initialized) {
        LOG_WARNING("AssetManager already initialized");
        return true;
    }

    dataPath = dataPath_;
    overridePath_ = dataPath + "/override";
    LOG_INFO("Initializing asset manager with data path: ", dataPath);

    setupFileCacheBudget();
    if (const size_t mb = parseEnvSizeMB("WOWEE_TEXTURE_PREPARATION_MB"))
        texturePreparations_.setBudget(mb * 1024 * 1024);

    std::string manifestPath = dataPath + "/manifest.json";
    if (!std::filesystem::exists(manifestPath)) {
        LOG_ERROR("manifest.json not found in: ", dataPath);
        LOG_ERROR("Run asset_extract to extract MPQ archives first");
        return false;
    }

    if (!manifest_.load(manifestPath)) {
        LOG_ERROR("Failed to load manifest");
        return false;
    }

    overrideDirExists_ = std::filesystem::is_directory(overridePath_);
    if (overrideDirExists_) {
        LOG_INFO("Override directory found: ", overridePath_);
    }

    initialized = true;
    LOG_INFO("Asset manager initialized: ", manifest_.getEntryCount(),
             " files indexed (file cache: ", fileCacheBudget / (1024 * 1024), " MB)");
    return true;
}

bool AssetManager::switchDataPath(const std::string& newDataPath) {
    if (newDataPath.empty()) return false;
    if (initialized && newDataPath == dataPath) return true;

    const std::string manifestPath = newDataPath + "/manifest.json";
    AssetManifest nextManifest;
    if (!std::filesystem::exists(manifestPath) || !nextManifest.load(manifestPath)) {
        LOG_ERROR("Cannot switch asset path; valid manifest not found in: ", newDataPath);
        return false;
    }

    clearCache();
    manifest_ = std::move(nextManifest);
    baseFallbackManifest_ = AssetManifest{};
    baseFallbackDataPath_.clear();
    dataPath = newDataPath;
    overridePath_ = dataPath + "/override";
    overrideDirExists_ = std::filesystem::is_directory(overridePath_);
    initialized = true;
    LOG_INFO("Switched asset manager to: ", dataPath, " (",
             manifest_.getEntryCount(), " files indexed)");
    return true;
}

void AssetManager::setupFileCacheBudget() {
    // A fixed budget, not a share of the memory free at startup. Three
    // quarters of half the free RAM, up to 12 GB, was what this cache was
    // given on a desktop - files the client had already parsed into models
    // and textures held a second time, in a process that died of bad_alloc
    // in a crowded city. What it keeps now is the files read for their own
    // sake; a gigabyte of those is the whole of a city's models many times.
#ifdef __ANDROID__
    // Android enforces a per-app limit far below the machine's memory and
    // kills the process rather than swapping when it is passed.
    constexpr size_t kDefaultBudgetMB = 384;
#else
    constexpr size_t kDefaultBudgetMB = 1024;
#endif
    constexpr size_t kMinBudgetBytes = 256ull * 1024ull * 1024ull;

    fileCacheBudget = kDefaultBudgetMB * 1024ull * 1024ull;
    if (const size_t envFixedMB = parseEnvSizeMB("WOWEE_FILE_CACHE_MB"); envFixedMB > 0) {
        fileCacheBudget = std::max(envFixedMB * 1024ull * 1024ull, kMinBudgetBytes);
        LOG_WARNING("Asset file cache fixed via WOWEE_FILE_CACHE_MB=", envFixedMB,
                    " (effective ", fileCacheBudget / (1024 * 1024), " MB)");
    }
}

void AssetManager::shutdown() {
    if (!initialized) {
        return;
    }

    LOG_INFO("Shutting down asset manager");
    const auto textures = texturePreparationStats();
    LOG_INFO("Texture preparation cache: ", textures.hits, " hits, ", textures.misses,
             " preparations, ", textures.joined, " joined, ", textures.evicted, " evictions, ",
             textures.oversized, " oversized, ", textures.bytes, " retained bytes / ",
             textures.budgetBytes, " budget, ", textures.entries, " entries");

    if (fileCacheHits + fileCacheMisses > 0) {
        float hitRate = static_cast<float>(fileCacheHits) / (fileCacheHits + fileCacheMisses) * 100.0f;
        LOG_INFO("File cache stats: ", fileCacheHits, " hits, ", fileCacheMisses, " misses (",
                 static_cast<int>(hitRate), "% hit rate), ", fileCacheStats().bytes / 1024 / 1024, " MB cached");
    }

    clearCache();
    initialized = false;
}

std::string AssetManager::resolveFile(const std::string& normalizedPath) const {
    std::string overridePath = resolveOverride(normalizedPath);
    if (!overridePath.empty()) return overridePath;
    // Held by the archive layer: there is a file, and it is not on disk.
    if (auto layer = archiveLayer(); layer && layer->contains(normalizedPath)) return {};
    return resolveExtracted(normalizedPath);
}

std::string AssetManager::resolveOverride(const std::string& normalizedPath) const {
    // Check override directory first (for HD upgrades, custom textures)
    if (overrideDirExists_) {
        const auto* entry = manifest_.lookup(normalizedPath);
        if (entry && !entry->filesystemPath.empty()) {
            std::string overrideFsPath = overridePath_ + "/" + entry->filesystemPath;
            if (looseFileExists(overrideFsPath)) {
                return overrideFsPath;
            }
        }
        // A pack is allowed to add as well as replace, and an addition has no
        // manifest entry to be resolved against: a later client's leaf atlas
        // arrives under a name this expansion has never had. So the path is
        // taken as the path. Without this, a model dropped in as an override
        // rendered white, because its own new textures were unreachable while
        // it was not.
        std::string overrideByPath = normalizedPath;
        std::replace(overrideByPath.begin(), overrideByPath.end(), '\\', '/');
        overrideByPath = overridePath_ + "/" + overrideByPath;
        if (looseFileExists(overrideByPath)) {
            return overrideByPath;
        }
    }
    return {};
}

std::string AssetManager::resolveExtracted(const std::string& normalizedPath) const {
    // Primary manifest
    std::string primaryPath = manifest_.resolveFilesystemPath(normalizedPath);
    if (!primaryPath.empty()) return primaryPath;

    // If a base-path fallback is configured (expansion-specific primary that only
    // holds DBC overrides), retry against the base extraction.
    if (!baseFallbackDataPath_.empty()) {
        std::string baseFallbackPath = baseFallbackManifest_.resolveFilesystemPath(normalizedPath);
        if (!baseFallbackPath.empty()) {
            baseFallbackHits_.fetch_add(1, std::memory_order_relaxed);
            return baseFallbackPath;
        }
    }

    // Last resort: some files (e.g. DBFilesClient\TransportAnimation.dbc) can end up
    // present on disk under dataPath without ever having been captured by whatever
    // extraction produced manifest.json. Try the loose file directly at its expected
    // location before giving up, so a missing manifest entry doesn't silently mean
    // "file doesn't exist" when it plainly does.
    std::string looseCandidate = normalizedPath;
    std::replace(looseCandidate.begin(), looseCandidate.end(), '\\', '/');
    looseCandidate = dataPath + "/" + looseCandidate;
    if (looseFileExists(looseCandidate)) {
        return looseCandidate;
    }
    return {};
}

std::shared_ptr<const ArchiveLayer> AssetManager::archiveLayer() const {
    std::shared_lock<std::shared_mutex> lock(archiveMutex_);
    return archiveLayer_;
}

void AssetManager::setArchiveLayer(std::shared_ptr<const ArchiveLayer> layer) {
    {
        std::lock_guard<std::shared_mutex> lock(archiveMutex_);
        archiveLayer_ = std::move(layer);
    }
    // After the swap, so that nothing read through the old layer survives it:
    // a read already under way may still finish against the old one, and the
    // generation this bumps keeps its result out of the caches.
    clearCache();
}

bool AssetManager::looseFileExists(const std::string& filesystemPath) const {
    {
        std::shared_lock<std::shared_mutex> lock(existsMutex_);
        auto it = existsCache_.find(filesystemPath);
        if (it != existsCache_.end()) return it->second;
    }
    // Probed outside the lock: two threads asking at once both stat the same
    // file and agree, which is cheaper than serializing every probe.
    const bool exists = LooseFileReader::fileExists(filesystemPath);
    std::lock_guard<std::shared_mutex> lock(existsMutex_);
    existsCache_.emplace(filesystemPath, exists);
    return exists;
}

bool AssetManager::setBaseFallbackPath(const std::string& basePath,
                                       const std::string& expansionId) {
    // Removing/changing the fallback changes texture sources as well as files.
    clearCache();
    if (baseFallbackHits_.load(std::memory_order_relaxed) > 0) {
        // Said on the way out rather than per lookup. Next to a warning about
        // an unlabelled or forced base, this is how much of what was on screen
        // came from it.
        LOG_INFO("AssetManager: previous base fallback '", baseFallbackDataPath_,
                 "' answered ", baseFallbackHits_.load(std::memory_order_relaxed),
                 " lookups");
    }
    baseFallbackDataPath_.clear();
    baseFallbackHits_.store(0, std::memory_order_relaxed);
    if (basePath.empty() || basePath == dataPath) return false;  // nothing to do
    std::string manifestPath = basePath + "/manifest.json";
    if (!std::filesystem::exists(manifestPath)) {
        LOG_DEBUG("AssetManager: base fallback manifest not found at ", manifestPath,
                  " - fallback disabled");
        return false;
    }
    if (!baseFallbackManifest_.load(manifestPath)) return false;

    const std::string& baseExpansion = baseFallbackManifest_.getExpansion();
    const char* forcedEnv = std::getenv("WOWEE_ASSET_BASE_FALLBACK");
    const bool forced = forcedEnv && forcedEnv[0] == '1';

    switch (decideBaseFallback(baseExpansion, expansionId, forced)) {
        case BaseFallbackDecision::Use:
            break;
        case BaseFallbackDecision::UseUnlabelled:
            // Nothing can be concluded, so it is used and said: an unlabelled
            // tree is how the wrong client's assets reach the screen without
            // anybody having chosen that.
            LOG_WARNING("AssetManager: base fallback '", basePath,
                        "' does not record which client it was extracted from, so it "
                        "cannot be checked against '", expansionId,
                        "'. Re-extract to label it. Files it holds and '", expansionId,
                        "' does not will be used as they are");
            break;
        case BaseFallbackDecision::Refuse:
            LOG_ERROR("AssetManager: base fallback '", basePath, "' was extracted from '",
                      baseExpansion, "' and the client is running '", expansionId,
                      "'. Refusing it: every file '", expansionId,
                      "' does not cover would otherwise be drawn from '", baseExpansion,
                      "' with nothing to say so. Extract '", expansionId,
                      "' into its own data root, or set WOWEE_ASSET_BASE_FALLBACK=1 "
                      "to use it anyway");
            return false;
        case BaseFallbackDecision::UseForced:
            LOG_WARNING("AssetManager: using base fallback '", basePath, "' from '",
                        baseExpansion, "' while running '", expansionId,
                        "' because WOWEE_ASSET_BASE_FALLBACK=1");
            break;
    }

    baseFallbackDataPath_ = basePath;
    LOG_INFO("AssetManager: base fallback path set to '", basePath, "' (",
             baseFallbackManifest_.getEntryCount(), " files, expansion '",
             baseExpansion.empty() ? std::string("unrecorded") : baseExpansion, "')");
    return true;
}

BLPImage AssetManager::loadTexture(const std::string& path, bool keepCompressed) {
    BLPImage image = loadTextureShared(path, keepCompressed)->image;
    // Value callers own mutable pixels (e.g. character magenta-key/composites).
    // The shared facts describe the immutable source, not their future edits.
    image.invalidateAlphaAnalysis();
    return image;
}

PreparedTextureRef AssetManager::loadTextureShared(const std::string& path, bool keepCompressed) {
    if (!initialized) {
        LOG_ERROR("AssetManager not initialized - texture is unavailable: ", path);
        return std::make_shared<const PreparedTexture>();
    }
    const std::string normalized = normalizePath(path);
    const bool bc = blockCompressionSupported();
    keepCompressed = keepCompressed && bc;
    // Device capability changes the source policy (DDS is unavailable without BC).
    const std::string key = normalized + (keepCompressed ? "|blocks" : bc ? "|rgba-bc" : "|rgba");
    return texturePreparations_.get(key, [&](uint64_t generation) -> PreparedTextureRef {
        PreparedTexture result;
        result.generation = generation;
        if (normalized.empty()) return std::make_shared<const PreparedTexture>(std::move(result));
        if (!keepCompressed && bc) {
            const auto stored = loadTextureShared(normalized, true);
            // A clear may separate this request from its nested source request.
            // Label decoded pixels with the source epoch, not the outer flight.
            result.generation = stored->generation;
            if (!stored->image.isBlockCompressed()) {
                // PNG/uncompressed BLP already is RGBA; share its allocation.
                return stored;
            } else {
                result.image.width = stored->image.width;
                result.image.height = stored->image.height;
                result.image.channels = stored->image.channels;
                result.image.format = stored->image.format;
                result.image.compression = stored->image.compression;
                result.image.data = BLPLoader::decodeBaseLevel(stored->image);
                // Preserve the existing diagnostic/fallback for unsupported DDS.
                if (result.image.data.empty()) result.image = loadTextureUncached(normalized, false);
            }
        } else {
            result.image = loadTextureUncached(normalized, keepCompressed);
        }
        if (result.image.isValid()) result.image.analyzeAlpha();
        return std::make_shared<const PreparedTexture>(std::move(result));
    }, [](const PreparedTexture& prepared) {
        const auto& image = prepared.image;
        size_t bytes = image.data.capacity() + image.mipmaps.capacity() * sizeof(std::vector<uint8_t>);
        for (const auto& mip : image.mipmaps) bytes += mip.capacity();
        return bytes;
    });
}

BLPImage AssetManager::loadTextureUncached(const std::string& path, bool keepCompressed) {
    ZoneScopedN("AssetManager::loadTexture");
    // Callers ask for the blocks to save the decompression and the memory. On a
    // GPU that cannot sample them the saving is a blank surface, so the answer
    // is no and the loader unpacks to RGBA8 instead.
    keepCompressed = keepCompressed && blockCompressionSupported();
    if (!initialized) {
        LOG_ERROR("AssetManager not initialized");
        return BLPImage();
    }

    std::string normalizedPath = normalizePath(path);

    LOG_DEBUG("Loading texture: ", normalizedPath);

    // Check for PNG override
    // Block-compressed override first, then the RGBA8 one. Both are read in
    // preference to the .blp beside them.
    BLPImage ddsImage = tryLoadDdsOverride(normalizedPath);
    if (ddsImage.isValid()) {
        // The blocks go only to a caller that asked for them. The rest read
        // RGBA8 from `data`, as the .blp beside it gives it them: handed the
        // blocks, the character renderer uploaded an empty `data` and drew
        // every upscaled glow on a weapon or a creature as a white card.
        if (keepCompressed || !ddsImage.isBlockCompressed()) return ddsImage;
        std::vector<uint8_t> pixels = BLPLoader::decodeBaseLevel(ddsImage);
        if (!pixels.empty()) {
            ddsImage.data = std::move(pixels);
            ddsImage.mipmaps.clear();
            ddsImage.mipLevels = 1;
            return ddsImage;
        }
        static std::mutex logMtx;
        static std::unordered_set<std::string> loggedUndecodable;
        std::lock_guard<std::mutex> lock(logMtx);
        if (loggedUndecodable.insert(normalizedPath).second) {
            LOG_WARNING("DDS override for ", normalizedPath,
                        " could not be decoded to RGBA8 - the .blp is used instead");
        }
    }

    BLPImage pngImage = tryLoadPngOverride(normalizedPath);
    if (pngImage.isValid()) {
        return pngImage;
    }

    // Not kept in the file cache: decoded and uploaded, the texture is the
    // GPU texture cache's from here on, and the file's bytes would only be a
    // second copy of it in system memory.
    std::vector<uint8_t> blpData = readFileNotKept(normalizedPath);
    if (blpData.empty()) {
        static std::mutex logMtx;
        static std::unordered_set<std::string> loggedMissingTextures;
        static bool missingTextureLogSuppressed = false;
        static const size_t kMaxMissingTextureLogKeys =
            parseEnvCount("WOWEE_TEXTURE_MISS_LOG_KEYS", 400);
        std::lock_guard<std::mutex> lock(logMtx);
        if (loggedMissingTextures.size() < kMaxMissingTextureLogKeys &&
            loggedMissingTextures.insert(normalizedPath).second) {
            LOG_WARNING("Texture not found: ", normalizedPath);
        } else if (!missingTextureLogSuppressed && loggedMissingTextures.size() >= kMaxMissingTextureLogKeys) {
            LOG_WARNING("Texture-not-found warning key cache reached ", kMaxMissingTextureLogKeys,
                        " entries; suppressing new unique texture-miss logs");
            missingTextureLogSuppressed = true;
        }
        return BLPImage();
    }

    BLPImage image = BLPLoader::load(blpData, keepCompressed);
    if (!image.isValid()) {
        static std::mutex logMtx;
        static std::unordered_set<std::string> loggedDecodeFails;
        static bool decodeFailLogSuppressed = false;
        static const size_t kMaxDecodeFailLogKeys =
            parseEnvCount("WOWEE_TEXTURE_DECODE_LOG_KEYS", 200);
        std::lock_guard<std::mutex> lock(logMtx);
        if (loggedDecodeFails.size() < kMaxDecodeFailLogKeys &&
            loggedDecodeFails.insert(normalizedPath).second) {
            LOG_ERROR("Failed to load texture: ", normalizedPath);
        } else if (!decodeFailLogSuppressed && loggedDecodeFails.size() >= kMaxDecodeFailLogKeys) {
            LOG_WARNING("Texture-decode warning key cache reached ", kMaxDecodeFailLogKeys,
                        " entries; suppressing new unique decode-failure logs");
            decodeFailLogSuppressed = true;
        }
        return BLPImage();
    }

    LOG_DEBUG("Loaded texture: ", normalizedPath, " (", image.width, "x", image.height, ")");
    return image;
}

std::string AssetManager::resolveSidecarPath(const std::string& normalizedPath,
                                             const char* extension) const {
    if (normalizedPath.size() < 4) return {};
    if (normalizedPath.substr(normalizedPath.size() - 4) != ".blp") return {};

    // The standard sidecar path first: the extracted .blp's directory, same
    // basename, the sidecar's extension.
    std::string fsPath = resolveFile(normalizedPath);
    if (!fsPath.empty() && fsPath.size() >= 4) {
        std::string candidate = fsPath.substr(0, fsPath.size() - 4) + extension;
        if (looseFileExists(candidate)) return candidate;
    }

    // Fallback: probe well-known custom-zone texture roots so that sidecar-only
    // assets ship without needing a phantom BLP manifest entry. Path is
    // forward-slash + lowercase to match the editor's export convention.
    std::string norm = normalizedPath;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    std::transform(norm.begin(), norm.end(), norm.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const std::string tail = norm.substr(0, norm.size() - 4) + extension;
    for (const char* root : {"custom_zones/textures/", "output/textures/"}) {
        std::string p = std::string(root) + tail;
        if (looseFileExists(p)) return p;
    }
    return {};
}

/// A block-compressed override, preferred to the PNG one.
///
/// A PNG arrives as RGBA8 and the renderer generates its mips on upload, which
/// costs four times the memory of the blocks it replaced and loses the one
/// thing worth writing a mip chain by hand for: a cutout's coverage. A .dds
/// sidecar hands over the blocks and the chain as they were made. On a device
/// that cannot sample BC at all - Mali and Adreno carry ASTC and ETC2 instead -
/// this declines, and the PNG or the original BLP answers instead.
BLPImage AssetManager::tryLoadDdsOverride(const std::string& normalizedPath) const {
    // WOWEE_NO_DDS=1 takes the shipped BLP instead, for telling an upscale
    // apart from the thing it is being blamed for. Something that looks wrong
    // and has a sidecar is two questions at once, and there was no way to ask
    // them separately without moving files out of the installation by hand.
    static const bool kDisabled = [] {
        const char* v = std::getenv("WOWEE_NO_DDS");
        return v && *v && *v != '0';
    }();
    if (kDisabled) return BLPImage();
    if (!blockCompressionSupported()) return BLPImage();
    const std::string ddsPath = resolveSidecarPath(normalizedPath, ".dds");
    if (ddsPath.empty()) return BLPImage();

    std::vector<uint8_t> ddsData = LooseFileReader::readFile(ddsPath);
    if (ddsData.empty()) {
        LOG_WARNING("DDS override exists but could not be read: ", ddsPath);
        return BLPImage();
    }
    BLPImage image = DdsLoader::load(ddsData);
    if (!image.isValid()) {
        LOG_WARNING("DDS override rejected, falling back: ", ddsPath);
        return BLPImage();
    }
    LOG_INFO("DDS override loaded: ", ddsPath, " (", image.width, "x", image.height,
             ", ", image.mipLevels, " mips)");
    return image;
}

BLPImage AssetManager::tryLoadPngOverride(const std::string& normalizedPath) const {
    const std::string pngPath = resolveSidecarPath(normalizedPath, ".png");
    if (pngPath.empty()) return BLPImage();

    int w, h, channels;
    unsigned char* pixels = stbi_load(pngPath.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        LOG_WARNING("PNG override exists but failed to load: ", pngPath);
        return BLPImage();
    }
    // Cap texture dimensions. WoW textures top out at 4K; stbi can return
    // 32K x 32K which would allocate 4GB on a malicious PNG.
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        LOG_WARNING("PNG override dimensions out of range (", w, "x", h, "): ", pngPath);
        stbi_image_free(pixels);
        return BLPImage();
    }

    BLPImage image;
    image.width = w;
    image.height = h;
    image.channels = 4;
    image.format = BLPFormat::BLP2;
    image.compression = BLPCompression::ARGB8888;
    image.data.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
    stbi_image_free(pixels);

    LOG_INFO("PNG override loaded: ", pngPath, " (", w, "x", h, ")");
    return image;
}

void AssetManager::setExpansionDataPath(const std::string& path) {
    expansionDataPath_ = path;
    LOG_INFO("Expansion data path for CSV DBCs: ", expansionDataPath_);
}

std::shared_ptr<DBCFile> AssetManager::loadDBC(const std::string& name) {
    ZoneScopedN("AssetManager::loadDBC");
    if (!initialized) {
        LOG_ERROR("AssetManager not initialized");
        return nullptr;
    }

    uint64_t generation = 0;
    if (auto cached = cachedDBC(name, &generation)) {
        LOG_DEBUG("DBC already loaded (cached): ", name);
        return cached;
    }

    LOG_DEBUG("Loading DBC: ", name);

    std::vector<uint8_t> dbcData;

    // Try binary DBC from extracted MPQs first (preferred source).
    std::string dbcPath = "DBFilesClient\\" + name;
    {
        // The parsed table is cached (dbcCache); its file's bytes are not.
        dbcData = readFileNotKept(normalizePath(dbcPath));
    }

    // If asset_extract was run with --emit-json-dbc, the DBC's directory
    // also contains a JSON sidecar. Use it when the binary is missing
    // (lets users run with PNG/JSON-only extractions for testing the
    // open-format end-to-end path). Server-mode never reads via this
    // code path, so private-server compat is unaffected.
    if (dbcData.empty()) {
        std::string normalizedDbc = normalizePath(dbcPath);
        std::string fsPath = resolveFile(normalizedDbc);
        if (!fsPath.empty() && fsPath.size() >= 4) {
            std::string sidecar = fsPath.substr(0, fsPath.size() - 4) + ".json";
            if (std::filesystem::exists(sidecar)) {
                std::ifstream jf(sidecar, std::ios::binary | std::ios::ate);
                if (jf) {
                    auto sz = jf.tellg();
                    if (sz > 0) {
                        dbcData.resize(static_cast<size_t>(sz));
                        jf.seekg(0);
                        jf.read(reinterpret_cast<char*>(dbcData.data()), sz);
                        LOG_INFO("Loading JSON DBC sidecar: ", sidecar);
                    }
                }
            }
        }
    }

    // Try Data/db/ directory (pre-extracted binary DBCs shared across expansions)
    if (dbcData.empty()) {
        // Expansion overlay first (e.g. Data/expansions/tbc/overlay/db/), then
        // expansion db/, then shared Data/db/.
        std::vector<std::string> dbDirs;
        if (!expansionDataPath_.empty())
            dbDirs.push_back(expansionDataPath_ + "/overlay/db");
        dbDirs.push_back(dataPath + "/db");
        dbDirs.push_back(dataPath + "/../../db");
        dbDirs.emplace_back("Data/db");
        // Try exact-case first, then case-insensitive scan (Linux is case-sensitive
        // but DBC filenames in Data/db/ are often all-lowercase).
        std::string nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        for (const auto& dir : dbDirs) {
            if (!std::filesystem::is_directory(dir)) continue;
            std::string exact = dir + "/" + name;
            std::string resolved;
            if (std::filesystem::exists(exact)) {
                resolved = exact;
            } else {
                for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string fn = entry.path().filename().string();
                    std::string fnLower = fn;
                    std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (fnLower == nameLower) {
                        resolved = entry.path().string();
                        break;
                    }
                }
            }
            if (resolved.empty()) continue;
            std::ifstream f(resolved, std::ios::binary | std::ios::ate);
            if (f) {
                auto size = f.tellg();
                if (size > 0) {
                    f.seekg(0);
                    dbcData.resize(static_cast<size_t>(size));
                    f.read(reinterpret_cast<char*>(dbcData.data()), size);
                    LOG_INFO("Loaded binary DBC from: ", resolved, " (", size, " bytes)");
                    break;
                }
            }
        }
    }

    // Check for JSON DBC from custom zones (wowee open format)
    if (dbcData.empty()) {
        std::string baseName = name;
        auto dot = baseName.rfind('.');
        if (dot != std::string::npos) baseName = baseName.substr(0, dot);
        for (const char* dir : {"custom_zones", "output"}) {
            if (!std::filesystem::exists(dir)) continue;
            for (auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_directory()) continue;
                std::string jsonPath = entry.path().string() + "/data/" + baseName + ".json";
                if (std::filesystem::exists(jsonPath)) {
                    std::ifstream jf(jsonPath, std::ios::binary | std::ios::ate);
                    if (jf) {
                        auto sz = jf.tellg();
                        if (sz > 0) {
                            dbcData.resize(static_cast<size_t>(sz));
                            jf.seekg(0);
                            jf.read(reinterpret_cast<char*>(dbcData.data()), sz);
                            LOG_INFO("Loading JSON DBC override: ", jsonPath);
                        }
                    }
                    break;
                }
            }
            if (!dbcData.empty()) break;
        }
    }

    // Fall back to expansion-specific CSV (e.g. Data/expansions/wotlk/db/Spell.csv)
    if (dbcData.empty() && !expansionDataPath_.empty()) {
        std::string baseName = name;
        auto dot = baseName.rfind('.');
        if (dot != std::string::npos) {
            baseName = baseName.substr(0, dot);
        }
        std::string csvPath = expansionDataPath_ + "/db/" + baseName + ".csv";
        if (std::filesystem::exists(csvPath)) {
            std::ifstream f(csvPath, std::ios::binary | std::ios::ate);
            if (f) {
                auto size = f.tellg();
                if (size > 0) {
                    f.seekg(0);
                    dbcData.resize(static_cast<size_t>(size));
                    f.read(reinterpret_cast<char*>(dbcData.data()), size);
                    LOG_INFO("Binary DBC not found, using CSV fallback: ", csvPath);
                }
            }
        }
    }

    if (dbcData.empty()) {
        if (auto alone = loadDbcOverridesAlone(name)) return cacheDBC(name, std::move(alone), generation);
        LOG_WARNING("DBC not found: ", name);
        return nullptr;
    }

    auto dbc = std::make_shared<DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load DBC: ", name);
        return nullptr;
    }
    mergeDbcOverrides(name, *dbc);

    dbc = cacheDBC(name, std::move(dbc), generation);
    LOG_INFO("Loaded DBC: ", name, " (", dbc->getRecordCount(), " records)");
    return dbc;
}

// The lookup and the insert each take cacheMutex, and the load between them
// does not: readFile takes the same lock, and it is not recursive. loadDBC is
// called off the main thread - the entity spawner reads CharSections on a
// worker - so an unlocked find could walk the table while an insert rehashed
// it.
std::shared_ptr<DBCFile> AssetManager::cachedDBC(const std::string& name,
                                                 uint64_t* generation) const {
    std::shared_lock<std::shared_mutex> lock(cacheMutex);
    if (generation) *generation = cacheGeneration_;
    auto it = dbcCache.find(name);
    return it != dbcCache.end() ? it->second : nullptr;
}

// Two threads can load the same table between their lookups and their
// inserts. The first one in is kept and both callers get it, so there is one
// copy of each table rather than whichever finished last.
std::shared_ptr<DBCFile> AssetManager::cacheDBC(const std::string& name,
                                                std::shared_ptr<DBCFile> dbc,
                                                uint64_t generation) {
    std::lock_guard<std::shared_mutex> lock(cacheMutex);
    if (generation != cacheGeneration_) return dbc;
    return dbcCache.emplace(name, std::move(dbc)).first->second;
}

// The archive layer's diff for a table. Merged here, before the table is
// cached, because every reader takes the table from the cache: GameHandler's
// spell names, the visual kits, the display infos - none of them knows a realm
// added to what it reads.
void AssetManager::mergeDbcOverrides(const std::string& name, DBCFile& dbc) const {
    auto layer = archiveLayer();
    if (!layer) return;
    const std::string overPath = normalizePath("DBFilesClient\\over_" + name);
    if (!layer->contains(overPath)) return;
    DBCFile over;
    if (!over.load(layer->read(overPath))) {
        LOG_WARNING("Realm DBC diff ", overPath, " is unreadable; ", name, " stays as shipped");
        return;
    }
    const uint32_t before = dbc.getRecordCount();
    if (dbc.mergeOverrides(over)) {
        LOG_INFO("Merged realm DBC diff into ", name, ": ", over.getRecordCount(), " records, ",
                 dbc.getRecordCount() - before, " of them new (", dbc.getRecordCount(), " in all)");
    }
}

std::shared_ptr<DBCFile> AssetManager::loadDbcOverridesAlone(const std::string& name) const {
    auto layer = archiveLayer();
    if (!layer) return nullptr;
    const std::string overPath = normalizePath("DBFilesClient\\over_" + name);
    if (!layer->contains(overPath)) return nullptr;
    auto dbc = std::make_shared<DBCFile>();
    if (!dbc->load(layer->read(overPath))) return nullptr;
    LOG_INFO("Using realm DBC diff ", overPath, " as ", name, ", which has no base table");
    return dbc;
}

std::shared_ptr<DBCFile> AssetManager::loadDBCOptional(const std::string& name) {
    uint64_t generation = 0;
    if (auto cached = cachedDBC(name, &generation)) return cached;

    // Try binary DBC
    std::vector<uint8_t> dbcData;
    {
        std::string dbcPath = "DBFilesClient\\" + name;
        // The parsed table is cached (dbcCache); its file's bytes are not.
        dbcData = readFileNotKept(normalizePath(dbcPath));
    }

    // Fall back to expansion-specific CSV
    if (dbcData.empty() && !expansionDataPath_.empty()) {
        std::string baseName = name;
        auto dot = baseName.rfind('.');
        if (dot != std::string::npos) baseName = baseName.substr(0, dot);
        std::string csvPath = expansionDataPath_ + "/db/" + baseName + ".csv";
        if (std::filesystem::exists(csvPath)) {
            std::ifstream f(csvPath, std::ios::binary | std::ios::ate);
            if (f) {
                auto size = f.tellg();
                if (size > 0) {
                    f.seekg(0);
                    dbcData.resize(static_cast<size_t>(size));
                    f.read(reinterpret_cast<char*>(dbcData.data()), size);
                    LOG_INFO("Binary DBC not found, using CSV fallback: ", csvPath);
                }
            }
        }
    }

    if (dbcData.empty()) {
        if (auto alone = loadDbcOverridesAlone(name)) return cacheDBC(name, std::move(alone), generation);
        // Expected on some expansions - log at debug level only.
        LOG_DEBUG("Optional DBC not found (expected on some expansions): ", name);
        return nullptr;
    }

    auto dbc = std::make_shared<DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load DBC: ", name);
        return nullptr;
    }
    mergeDbcOverrides(name, *dbc);

    dbc = cacheDBC(name, std::move(dbc), generation);
    LOG_INFO("Loaded optional DBC: ", name, " (", dbc->getRecordCount(), " records)");
    return dbc;
}
bool AssetManager::fileExists(const std::string& path) const {
    if (!initialized) {
        return false;
    }
    std::string normalized = normalizePath(path);
    // Resolved the same way a read is, not looked up in the primary manifest.
    //
    // A read walks override, primary manifest, base fallback manifest, then the
    // loose file; this asked the primary manifest alone. With one asset source
    // the two agree, which is why it stood. The moment an expansion overlay
    // becomes the primary - an overlay holding a few thousand models over a
    // two-hundred-thousand file base - this answered NO for every file in the
    // base game, while the read that follows would have found it.
    //
    // Fifty-eight callers ask this before deciding what to read, so every one
    // of them took the wrong branch at once: a weapon texture that is present
    // under Weapon\ was declared missing and looked for under Shield\, where
    // it has never been, and the weapon drew white.
    if (!resolveOverride(normalized).empty()) return true;
    if (auto layer = archiveLayer(); layer && layer->contains(normalized)) return true;
    return !resolveExtracted(normalized).empty();
}

std::vector<uint8_t> AssetManager::readFile(const std::string& path) const {
    // A copy for a caller that keeps or changes the bytes, taken outside the
    // cache's lock.
    if (FileBytes bytes = readFileShared(path)) return *bytes;
    return {};
}

AssetManager::FileBytes AssetManager::readFileShared(const std::string& path) const {
    if (!initialized) {
        return nullptr;
    }
    const std::string normalized = normalizePath(path);
    uint64_t generation = 0;
    if (FileBytes hit = cachedFile(normalized, generation)) return hit;
    // A miss of the files read to be kept; a texture's or DBC's read is not.
    fileCacheMisses++;
    std::vector<uint8_t> data = readFromSource(normalized);
    if (data.empty()) return nullptr;
    return keepFile(normalized, std::move(data), generation);
}

std::vector<uint8_t> AssetManager::readFileNotKept(const std::string& normalized) const {
    if (!initialized) {
        return {};
    }
    uint64_t generation = 0;
    if (FileBytes hit = cachedFile(normalized, generation)) return *hit;
    return readFromSource(normalized);
}

AssetManager::FileBytes AssetManager::cachedFile(const std::string& normalized,
                                                 uint64_t& generation) const {
    std::unique_lock<std::mutex> lock(fileCacheMutex_);
    generation = fileCacheGeneration_;
    const auto it = fileCache.find(normalized);
    if (it == fileCache.end()) return nullptr;
    // Most recently read: to the front, out of the way of eviction.
    fileCacheLru_.splice(fileCacheLru_.begin(), fileCacheLru_, it->second);
    FileBytes data = it->second->data;
    lock.unlock();
    fileCacheHits++;
    return data;
}

void AssetManager::evictFilesFor(size_t incoming) const {
    while (!fileCacheLru_.empty() && fileCacheTotalBytes + incoming > fileCacheBudget) {
        const CachedFile& oldest = fileCacheLru_.back();
        fileCacheTotalBytes -= oldest.data->size();
        fileCache.erase(oldest.path);
        fileCacheLru_.pop_back();
    }
}

AssetManager::FileBytes AssetManager::keepFile(const std::string& normalized,
                                               std::vector<uint8_t>&& data,
                                               uint64_t generation) const {
    auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    const size_t fileSize = bytes->size();
    std::lock_guard<std::mutex> lock(fileCacheMutex_);
    // Too large to keep without pushing out half the cache, or read through
    // a layer that has since been replaced: handed out, not kept.
    if (fileSize >= fileCacheBudget / 2 || generation != fileCacheGeneration_) return bytes;
    // Another thread read the same file meanwhile and kept it: share that copy.
    if (const auto it = fileCache.find(normalized); it != fileCache.end()) {
        fileCacheLru_.splice(fileCacheLru_.begin(), fileCacheLru_, it->second);
        return it->second->data;
    }
    evictFilesFor(fileSize);
    fileCacheLru_.push_front({normalized, bytes});
    fileCache.emplace(normalized, fileCacheLru_.begin());
    fileCacheTotalBytes += fileSize;
    return bytes;
}

std::vector<uint8_t> AssetManager::readFromSource(const std::string& normalized) const {
    // The override dir, then the archive layer, then what was extracted.
    std::vector<uint8_t> data;
    std::string fsPath = resolveOverride(normalized);
    if (fsPath.empty()) {
        if (auto layer = archiveLayer(); layer && layer->contains(normalized)) {
            data = layer->read(normalized);
            // A damaged copy in an archive is not a missing file: the
            // extracted one it replaced is still better than nothing.
            if (data.empty()) LOG_WARNING("Archive entry unreadable, using the extracted file: ", normalized);
        }
        if (data.empty()) fsPath = resolveExtracted(normalized);
    }
    if (data.empty()) {
        if (fsPath.empty()) {
            return {};
        }
        data = LooseFileReader::readFile(fsPath);
        if (data.empty()) {
            LOG_WARNING("Manifest entry exists but file unreadable: ", fsPath);
            return data;
        }
    }
    return data;
}

void AssetManager::setFileCacheBudget(size_t bytes) {
    std::lock_guard<std::mutex> lock(fileCacheMutex_);
    fileCacheBudget = bytes;
    evictFilesFor(0);
}

std::vector<uint8_t> AssetManager::readFileOptional(const std::string& path) const {
    if (!initialized) {
        return {};
    }
    if (!fileExists(path)) {
        return {};
    }
    return readFile(path);
}

core::CacheStats AssetManager::fileCacheStats() const {
    std::lock_guard<std::mutex> lock(fileCacheMutex_);
    return {fileCache.size(), fileCacheTotalBytes, fileCacheBudget};
}

size_t AssetManager::dbcCacheEntries() const {
    std::shared_lock<std::shared_mutex> lock(cacheMutex);
    return dbcCache.size();
}

void AssetManager::clearDBCCache() {
    std::lock_guard<std::shared_mutex> lock(cacheMutex);
    dbcCache.clear();
    ++cacheGeneration_;
    LOG_INFO("Cleared DBC cache");
}

void AssetManager::clearCache() {
    {
        std::lock_guard<std::shared_mutex> existsLock(existsMutex_);
        existsCache_.clear();
    }
    {
        std::lock_guard<std::mutex> fileLock(fileCacheMutex_);
        ++fileCacheGeneration_;
        fileCache.clear();
        fileCacheLru_.clear();
        fileCacheTotalBytes = 0;
    }
    std::lock_guard<std::shared_mutex> lock(cacheMutex);
    ++cacheGeneration_;
    dbcCache.clear();
    // Publish the preparation generation after all source-cache invalidations.
    // A reader between those clears still belongs to the old preparation epoch.
    texturePreparations_.clear();
    LOG_INFO("Cleared asset cache (DBC + files + texture preparation)");
}
std::string AssetManager::normalizePath(const std::string& path) const {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Reject path traversal sequences
    if (normalized.find("..\\") != std::string::npos ||
        normalized.find("../") != std::string::npos ||
        normalized == "..") {
        LOG_WARNING("Path traversal rejected: ", path);
        return {};
    }

    return normalized;
}

} // namespace pipeline
} // namespace wowee
