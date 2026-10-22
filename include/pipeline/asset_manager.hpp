#pragma once

#include "core/cache_stats.hpp"
#include "pipeline/archive_layer.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/prepared_texture.hpp"
#include "pipeline/preparation_cache.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manifest.hpp"
#include "pipeline/loose_file_reader.hpp"
#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

namespace wowee {
namespace pipeline {

/**
 * AssetManager - Unified interface for loading WoW assets
 *
 * Reads pre-extracted loose files indexed by manifest.json.
 * Supports an override directory (Data/override/) checked before the manifest
 * for HD textures, custom content, or mod overrides.
 * Use the asset_extract tool to extract MPQ archives first.
 * All reads are fully parallel (no serialization mutex needed).
 */
class AssetManager {
public:
    AssetManager();
    ~AssetManager();

    /**
     * Initialize asset manager
     * @param dataPath Path to directory containing manifest.json and extracted assets
     * @return true if initialization succeeded
     */
    bool initialize(const std::string& dataPath);

    /**
     * Replace the active manifest without replacing the AssetManager object.
     * Intended for expansion changes from the login screen, before world work
     * is active. Existing file/DBC caches are cleared on success.
     */
    bool switchDataPath(const std::string& newDataPath);

    /**
     * Shutdown and cleanup
     */
    void shutdown();

    /**
     * Check if asset manager is initialized
     */
    [[nodiscard]] bool isInitialized() const { return initialized; }
    [[nodiscard]] const std::string& getDataPath() const { return dataPath; }
    [[nodiscard]] const AssetManifest& getManifest() const { return manifest_; }

    /**
     * Load a BLP texture
     * @param path Virtual path to BLP file (e.g., "Textures\\Minimap\\Background.blp")
     * @return BLP image (check isValid())
     */
    /// keepCompressed asks the loader to leave a DXT texture in its blocks.
    /// A PNG override is decoded regardless - it has no blocks to keep.
    BLPImage loadTexture(const std::string& path, bool keepCompressed = false);
    /// Shared immutable image + alpha facts, deduplicated across worker jobs.
    /// Stored and RGBA are distinct outputs; RGBA reuses the stored source.
    /// An invalid image is a completed failure until eviction/clearCache.
    [[nodiscard]] PreparedTextureRef loadTextureShared(const std::string& path, bool keepCompressed = false);
    using TexturePreparationStats = PreparationCache<PreparedTexture>::Stats;
    [[nodiscard]] TexturePreparationStats texturePreparationStats() const { return texturePreparations_.stats(); }
    void setTexturePreparationBudget(size_t bytes) { texturePreparations_.setBudget(bytes); }

    /**
     * Set expansion-specific data path for CSV DBC lookup.
     * When set, loadDBC() checks expansionDataPath/db/Name.csv before
     * falling back to the manifest (binary DBC from extracted MPQs).
     */
    void setExpansionDataPath(const std::string& path);

    /**
     * Set a base data path to fall back to when the primary manifest does not
     * contain a requested file. Call this when the primary dataPath is an
     * expansion-specific subset (e.g. Data/expansions/wotlk/) that only holds
     * overrides rather than the full world asset set.
     *
     * The fallback is how a thin expansion tree works at all: the wotlk tree
     * here holds 18,943 files against the base tree's 199,468, so nine assets
     * in ten are answered by the base. It is also how one expansion's assets
     * come to stand in for another's, because it is resolved per file with no
     * check on where the file came from, and a borrowed file is not an error
     * anywhere. Cataclysm is the case that makes this visible rather than
     * merely wrong: its Azeroth is the sundered one, so a tile it does not
     * cover is drawn as the old world and nothing says so.
     *
     * So the two trees are compared. A base extracted from a different client
     * than the one being run is refused, and named, rather than used. A base
     * whose manifest predates the `expansion` field cannot be compared, so it
     * is used and said once. WOWEE_ASSET_BASE_FALLBACK=1 forces a refused one
     * back on for whoever is deliberately mixing trees.
     *
     * @param basePath   Path to the base extraction (Data/) with a manifest.json
     * @param expansionId The expansion actually being run, to compare against
     * @return whether the fallback is now armed
     */
    bool setBaseFallbackPath(const std::string& basePath,
                             const std::string& expansionId);

    /// How many lookups have been answered by the base fallback rather than by
    /// the expansion's own tree. Nine in ten is normal for a thin expansion
    /// tree and says nothing on its own; it is the count next to a mismatch
    /// that says how much of what is on screen came from the wrong client.
    [[nodiscard]] uint64_t getBaseFallbackHits() const {
        return baseFallbackHits_.load(std::memory_order_relaxed);
    }

    /**
     * Serve the files `layer` holds in preference to the extracted data - a
     * realm's patch archives, which replace models, textures and whole tables
     * of the client they patch. nullptr takes it away again.
     *
     * The override directory still comes first: what the player put there is
     * theirs. Beneath it the layer answers for every path it holds, and the
     * manifest, the base fallback and loose files only for the rest.
     *
     * A layer holding DBFilesClient\over_<Name>.dbc changes <Name>.dbc too:
     * loadDBC merges the diff into the table by record ID.
     *
     * Everything cached is forgotten, since any of it may now come from
     * somewhere else. What callers built from earlier reads is theirs to
     * rebuild.
     */
    void setArchiveLayer(std::shared_ptr<const ArchiveLayer> layer);
    [[nodiscard]] bool hasArchiveLayer() const { return archiveLayer() != nullptr; }

    /**
     * Load a DBC file
     * @param name DBC file name (e.g., "Map.dbc")
     * @return Loaded DBC file (check isLoaded())
     */
    std::shared_ptr<DBCFile> loadDBC(const std::string& name);

    /**
     * Load a DBC file that is optional (not all expansions ship it).
     * Returns nullptr quietly (debug-level log only) when the file is absent.
     * @param name DBC file name (e.g., "Item.dbc")
     * @return Loaded DBC file, or nullptr if not available
     */
    std::shared_ptr<DBCFile> loadDBCOptional(const std::string& name);


    /**
     * Check if a file exists
     * @param path Virtual file path
     * @return true if file exists
     */
    [[nodiscard]] bool fileExists(const std::string& path) const;

    /// A file's bytes as the file cache holds them, shared rather than copied.
    using FileBytes = std::shared_ptr<const std::vector<uint8_t>>;

    /**
     * Read raw file data, as a copy the caller owns.
     * @param path Virtual file path
     * @return File contents (empty if not found)
     */
    [[nodiscard]] std::vector<uint8_t> readFile(const std::string& path) const;

    /// Read a file to parse it: the bytes the file cache keeps, handed out
    /// without a copy, and kept alive by the pointer after the cache lets them
    /// go. Null when the file is not there or cannot be read. Any thread.
    [[nodiscard]] FileBytes readFileShared(const std::string& path) const;

    /**
     * Read optional file data without warning spam.
     * Intended for probe-style lookups (e.g. external .anim variants).
     * @param path Virtual file path
     * @return File contents (empty if not found)
     */
    [[nodiscard]] std::vector<uint8_t> readFileOptional(const std::string& path) const;

    /**
     * Get file cache stats
     */
    [[nodiscard]] size_t getFileCacheSize() const { return fileCacheTotalBytes; }
    /// The files kept in memory once read, in bytes, against the budget the
    /// least recently used are evicted to stay under. Any thread.
    [[nodiscard]] core::CacheStats fileCacheStats() const;
    /// The DBC tables kept parsed. Their bytes are not counted.
    [[nodiscard]] size_t dbcCacheEntries() const;
    [[nodiscard]] size_t getFileCacheHits() const { return fileCacheHits; }
    [[nodiscard]] size_t getFileCacheMisses() const { return fileCacheMisses; }
    /// The bytes the file cache may hold before it lets the least recently
    /// read files go; a file over half of it is read and not kept. Replaces
    /// the default (1 GB, or WOWEE_FILE_CACHE_MB) and evicts down to it now.
    void setFileCacheBudget(size_t bytes);

    /**
     * Clear all cached resources
     */
    void clearCache();

    /**
     * Clear only DBC cache (forces reload on next loadDBC call)
     */
    void clearDBCCache();


    /**
     * Resolve a normalized WoW path to its on-disk location. Checks the
     * override directory first, then the manifest, then the base-fallback
     * manifest. Public so callers (e.g. terrain_manager probing for
     * sidecar files like .whm/.wot/.woc next to a .adt) can locate the
     * extracted file's directory without reading it.
     *
     * A path the archive layer answers for has no file on disk, and "" is
     * returned for it even though it exists: the extracted file it replaces
     * is not the one being read, and neither are the sidecars beside that.
     * @return absolute or relative fs path, or "" if not found
     */
    [[nodiscard]] std::string resolveFile(const std::string& normalizedPath) const;

private:
    bool initialized = false;
    std::string dataPath;
    std::string expansionDataPath_;  // e.g. "Data/expansions/wotlk"
    std::string overridePath_;       // e.g. "Data/override"
    bool overrideDirExists_ = false; // probed once per data path

    /// Whether a file is on disk, asked of the disk once per path.
    ///
    /// Every resolve used to stat the override folder twice - present or not
    /// - and a path the manifest lacks a third time, and each texture load
    /// probed for its sidecars on top. The assets do not change while the game
    /// runs, but the questions repeat endlessly: every NPC and object spawned
    /// asks after the same few hundred paths, on the main thread, from the
    /// file system. clearCache forgets the answers.
    [[nodiscard]] bool looseFileExists(const std::string& filesystemPath) const;
    mutable std::shared_mutex existsMutex_;
    mutable std::unordered_map<std::string, bool> existsCache_;

    // Base manifest (loaded from dataPath/manifest.json)
    AssetManifest manifest_;
    PreparationCache<PreparedTexture> texturePreparations_{256ull * 1024 * 1024};
    BLPImage loadTextureUncached(const std::string& path, bool keepCompressed);

    // Optional base-path fallback: used when manifest_ doesn't contain a file.
    // Populated by setBaseFallbackPath(); ignored if baseFallbackDataPath_ is empty.
    std::string    baseFallbackDataPath_;
    AssetManifest  baseFallbackManifest_;
    // Counted from resolveAssetPath, which is const and takes no lock while
    // several threads stream assets through it, so relaxed atomic rather than
    // a plain mutable: nothing orders against this and a torn count would be
    // a data race for a diagnostic.
    mutable std::atomic<uint64_t> baseFallbackHits_{0};

    // (resolveFile moved to public - declaration above.)
    // Its two halves: the override directory, and what was extracted beneath
    // the archive layer - manifest, base fallback, loose file.
    [[nodiscard]] std::string resolveOverride(const std::string& normalizedPath) const;
    [[nodiscard]] std::string resolveExtracted(const std::string& normalizedPath) const;

    // Set on the main thread while workers read through it, so it is copied
    // out under a lock and used from the copy.
    [[nodiscard]] std::shared_ptr<const ArchiveLayer> archiveLayer() const;
    mutable std::shared_mutex archiveMutex_;
    std::shared_ptr<const ArchiveLayer> archiveLayer_;

    /// The archive layer's DBFilesClient\over_<name>, merged into `dbc`.
    void mergeDbcOverrides(const std::string& name, DBCFile& dbc) const;
    /// The archive layer's over_<name> on its own, for a table with no base.
    [[nodiscard]] std::shared_ptr<DBCFile> loadDbcOverridesAlone(const std::string& name) const;

    // dbcCache under cacheMutex. The load between them runs unlocked.
    //
    // A load can straddle setArchiveLayer: read half from the old layer,
    // cached after the clear. The generation, bumped with every clear, keeps
    // such a result out of the cache; the caller that asked still gets it.
    [[nodiscard]] std::shared_ptr<DBCFile> cachedDBC(const std::string& name,
                                                     uint64_t* generation = nullptr) const;
    std::shared_ptr<DBCFile> cacheDBC(const std::string& name, std::shared_ptr<DBCFile> dbc,
                                      uint64_t generation);
    // THREAD-SAFE: protected by cacheMutex.
    uint64_t cacheGeneration_ = 0;

    // Guards dbcCache and cacheGeneration_. Shared lock for a cache hit,
    // exclusive for inserts and clears.
    mutable std::shared_mutex cacheMutex;
    // THREAD-SAFE: protected by cacheMutex (exclusive lock for writes).
    std::unordered_map<std::string, std::shared_ptr<DBCFile>> dbcCache;

    // The file cache: least recently read at the back of the list, the map
    // pointing into it, so a hit moves its file to the front and an eviction
    // takes the back, both in constant time. A hit reorders the list, so even
    // a lookup takes the lock exclusively - for a hash lookup and a splice;
    // the bytes are shared out, never copied under it.
    struct CachedFile {
        std::string path;
        FileBytes data;
    };
    using FileLru = std::list<CachedFile>;
    // THREAD-SAFE: everything from here to fileCacheGeneration_ is protected
    // by fileCacheMutex_.
    mutable std::mutex fileCacheMutex_;
    mutable FileLru fileCacheLru_;
    mutable std::unordered_map<std::string, FileLru::iterator> fileCache;
    mutable size_t fileCacheTotalBytes = 0;
    /// 1 GB, or WOWEE_FILE_CACHE_MB, or setFileCacheBudget.
    size_t fileCacheBudget = 1024 * 1024 * 1024;
    /// Bumped by every clear, so a read that straddles one is not cached.
    uint64_t fileCacheGeneration_ = 0;
    // THREAD-SAFE: atomic - incremented from any thread after releasing the lock.
    mutable std::atomic<size_t> fileCacheHits{0};
    mutable std::atomic<size_t> fileCacheMisses{0};

    void setupFileCacheBudget();
    /// Let the least recently read files go until `incoming` more bytes fit.
    /// fileCacheMutex_ held.
    void evictFilesFor(size_t incoming) const;

    /// The cached bytes of a file, moved to the front of the list; null on a
    /// miss. `generation` is the cache's, to hand to keepFile after the read.
    [[nodiscard]] FileBytes cachedFile(const std::string& normalizedPath, uint64_t& generation) const;
    /// The file from where it lives - the override dir, the archive layer,
    /// what was extracted - bypassing the cache. Empty when it is not there.
    [[nodiscard]] std::vector<uint8_t> readFromSource(const std::string& normalizedPath) const;
    /// The bytes just read, kept for the next read when they fit the budget.
    FileBytes keepFile(const std::string& normalizedPath, std::vector<uint8_t>&& data,
                       uint64_t generation) const;
    /// A file whose bytes are not kept, because what they become is held
    /// elsewhere - a texture on the GPU, a parsed DBC - and they would only be
    /// a second copy. Served from the cache if something else kept it.
    [[nodiscard]] std::vector<uint8_t> readFileNotKept(const std::string& normalizedPath) const;

    /**
     * Try to load a PNG override for a BLP path.
     * Returns valid BLPImage if PNG found, invalid otherwise.
     */
    /// Where a sidecar of this extension sits for a normalized .blp path, or
    /// empty when there is none. Shared by the overrides below so they agree
    /// on where to look.
    [[nodiscard]] std::string resolveSidecarPath(const std::string& normalizedPath,
                                                 const char* extension) const;
    [[nodiscard]] BLPImage tryLoadDdsOverride(const std::string& normalizedPath) const;
    [[nodiscard]] BLPImage tryLoadPngOverride(const std::string& normalizedPath) const;

    /**
     * Normalize path for case-insensitive lookup
     */
    [[nodiscard]] std::string normalizePath(const std::string& path) const;
};

} // namespace pipeline
} // namespace wowee
