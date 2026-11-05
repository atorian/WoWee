#pragma once

// The world's textures made ready off the frame: read, decoded or derived
// where they must be, created on the GPU and their copies sent by workers of
// their own, and put in the shared cache only once those copies have run.
// The frame finds a texture there resident or not at all; it never reads a
// file, derives a map, creates an image or waits for a copy (AGENTS.md,
// "Loading in the world never holds a frame").
//
// Nothing waits for the GPU. A worker takes the jobs waiting, up to a batch,
// records all their copies into one upload batch, sends it and goes on to the
// next; the frame, which already polls every batch, puts the textures of
// those that have run in the cache (publish). A fence waited for per texture,
// on two shared I/O threads, held a city's thousands of textures in a queue
// for most of a minute.
//
// The one way a renderer asks for a cache entry it does not hold - a model's
// stored texture, a building's normal and height map. What waits for one (a
// model's batches, a building's load) asks again each frame until it is in or
// has failed.

#include "rendering/texture_cache.hpp"
#include "rendering/texture_upload.hpp"
#include "rendering/vk_context.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wowee::pipeline {
class AssetManager;
}  // namespace wowee::pipeline

namespace wowee::rendering {

class TextureLoads {
public:
    enum class State {
        /// In the cache: `out` holds it.
        Resident,
        /// Being made on a worker; ask again.
        Loading,
        /// It could not be made, as the worker said. No per-frame retry.
        Failed,
    };

    /// Runs on a worker inside its upload batch: the textures it made, the
    /// first the one asked for (none when it could not be made, having said
    /// why). Each goes in the cache under its own key once the batch has run.
    using Make = std::function<std::vector<PreparedTexture>()>;

    TextureLoads(VkContext& context, pipeline::AssetManager& assets);
    /// Drops the loads not begun; those sent are waited for, as the textures
    /// they copy into go with them.
    ~TextureLoads();
    TextureLoads(const TextureLoads&) = delete;
    TextureLoads& operator=(const TextureLoads&) = delete;

    /// `key` resident in `out`, or `make` (called here, only when a load
    /// starts) queued for a worker. Any thread.
    State request(const TextureKey& key, TextureRef& out, const std::function<Make()>& make);

    /// Bind once, including while loading. All copies resolve together after
    /// the upload fence. The maker factory only captures small job metadata;
    /// reading, decoding, derived pixels and GPU preparation run on a worker.
    TextureRef reference(const TextureKey& key, TextureRef fallback, const std::function<Make()>& make);
    TextureRef stored(const std::string& path, TextureRef fallback = {});
    TextureRef interfaceTexture(const std::string& path, InterfaceAlpha alpha = InterfaceAlpha::Original);
    /// Dataset changes happen outside gameplay, after UI consumers join work.
    void beginInterfaceDataset() { ++interfaceGeneration_; }
    [[nodiscard]] uint64_t revision(TextureForm form) const {
        return revisions_[static_cast<size_t>(form)].load();
    }

    /// A stored texture - DXT blocks as the file keeps them, what models
    /// sample - by its normalized path (normalizeTexturePath). A loader
    /// thread that knows a model is coming asks for its textures ahead of it.
    State requestStored(const std::string& key, TextureRef& out);

    /// Reads the fences of the batches sent, puts the textures of every one
    /// that has run in the cache, and says the loads of one that failed
    /// failed. Once a frame, on the thread that asks, before it asks again.
    void publish();
    /// Loading screens, teardown and dataset changes only. Joins CPU work,
    /// waits for uploads and publishes their answers before assets may change.
    void drain();
    void resetDataset();

    [[nodiscard]] VkContext& context() const { return context_; }
    [[nodiscard]] pipeline::AssetManager& assets() const { return assets_; }

private:
    /// A load running, failed, or made under another key than it is asked
    /// for by. One made under its own key leaves no record: the cache is it.
    struct Load {
        State state = State::Loading;
        /// Resident: the key the worker's texture is cached under.
        TextureKey madeAs;
        TextureRef reference;
    };
    struct Job {
        TextureKey key;
        Make make;
    };
    /// A batch sent, and what goes in the cache once it has run.
    struct Sent {
        VkContext::UploadCompletion completion;
        std::vector<TextureKey> keys;
        std::vector<std::vector<PreparedTexture>> made;
    };

    void work();
    State requestLocked(const TextureKey& key, TextureRef& out, const std::function<Make()>& make);
    Make makeStored(const TextureKey& key);
    /// With the lock held.
    void finish(const TextureKey& key, const TextureRef& made);

    VkContext& context_;
    pipeline::AssetManager& assets_;
    std::mutex mutex_;
    /// Signalled as a job is queued, and to stop.
    std::condition_variable queued_;
    std::condition_variable idle_;
    size_t working_ = 0;
    std::unordered_map<TextureKey, Load, TextureKeyHash> loads_;
    std::deque<Job> queue_;
    std::vector<Sent> sent_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    std::array<std::atomic<uint64_t>, kTextureFormCount> revisions_{};
    std::atomic<uint64_t> interfaceGeneration_{0};
};

}  // namespace wowee::rendering
