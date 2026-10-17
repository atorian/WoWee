#pragma once

#include "rendering/normal_map_encoding.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace rendering {

/// Builds normal/height maps from textures on a few worker threads of its
/// own, nearest first, with a queue of bounded length.
///
/// Requests carry no pixels: each names a key and a source the worker runs to
/// get the RGBA8 texture to derive from (reading the file again), so a queued
/// request costs a few bytes and one dropped for a nearer one costs nothing to
/// ask for again. When the queue is full a request nearer than the farthest
/// queued one takes its place; otherwise it is refused and its owner asks
/// again later. What is finished waits for the owner to take it and upload it.
///
/// Replaces a detached thread per texture, which in a crowded city started
/// thread after thread until starting one more failed.
class NormalMapBuilder {
public:
    struct Pixels {
        std::vector<uint8_t> rgba;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    /// Runs on a worker: the texture to derive from, or nothing if it cannot
    /// be read.
    using Source = std::function<std::optional<Pixels>()>;
    /// Runs on a worker: the normal/height map of `rgba`, encoded for the GPU
    /// with its mips and the variance of its heights. Invalid on failure.
    using Derive = std::function<EncodedNormalMap(std::vector<uint8_t> rgba, uint32_t width, uint32_t height)>;

    struct Request {
        std::string key;
        /// Lower is sooner: the distance to the nearest thing drawn with it.
        float priority = 0.0f;
        Source source;
    };
    struct Result {
        std::string key;
        /// The normal in rgb, the height in a, ready to upload.
        EncodedNormalMap map;
        /// The source could not be read or the map not derived. Said in the
        /// log by the builder, once per key; the owner stops asking.
        bool failed = false;
    };

    enum class Submitted {
        Queued,
        /// Queued, running or finished already; its priority is updated.
        AlreadyThere,
        /// The queue is full of nearer requests.
        Refused,
    };

    struct Stats {
        size_t queued = 0;
        size_t running = 0;
        size_t finished = 0;
        /// Queued requests that gave their place to nearer ones.
        uint64_t displaced = 0;
        uint64_t refused = 0;
    };

    NormalMapBuilder(size_t workers, size_t capacity, Derive derive);
    ~NormalMapBuilder();
    NormalMapBuilder(const NormalMapBuilder&) = delete;
    NormalMapBuilder& operator=(const NormalMapBuilder&) = delete;

    Submitted submit(Request request);
    /// Whether `key` is queued, being built or built and not taken.
    [[nodiscard]] bool contains(const std::string& key) const;
    /// Up to `max` finished maps, in the order they finished.
    [[nodiscard]] std::vector<Result> takeFinished(size_t max);
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] size_t capacity() const { return capacity_; }

private:
    void work();

    const size_t capacity_;
    const Derive derive_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    /// Queued requests, unordered: the queue is short and the nearest is
    /// found when a worker takes one, after priorities may have changed.
    std::vector<Request> queued_;
    /// Every key queued, running or finished and not taken.
    std::unordered_set<std::string> present_;
    size_t running_ = 0;
    std::deque<Result> finished_;
    uint64_t displaced_ = 0;
    uint64_t refused_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace rendering
} // namespace wowee
