#pragma once

// Every unit model the client draws, parsed once for its file.
//
// A unit's model - a race's body, a creature, a weapon, a helm, an enchant's
// glow - is its M2, its .skin and every sequence kept in .anim files beside
// it. Parsing one is the costly part of showing it: a race's body is a
// million words of keys in two hundred .anim files, sixty milliseconds of
// it. The target portrait parsed the target's model again on every click, on
// the frame thread, while the world had it parsed already.
//
// This holds each file's parse, made on an IO worker, for everyone who draws
// it. The frame asks with request(), which never parses: it says the model is
// on its way, and hands it over once it is. A worker that wants one now asks
// with load(), which parses on the calling thread when no one has yet. What is
// handed out is never changed; a consumer that needs to change it copies it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wowee {
namespace pipeline {

class AssetManager;
struct M2Model;

class M2ModelCache {
public:
    /// Parsed models are kept up to `budgetBytes`, the least recently asked
    /// for let go first; one being handed out stays alive with its holder.
    M2ModelCache(AssetManager& assets, size_t budgetBytes);
    ~M2ModelCache();
    M2ModelCache(const M2ModelCache&) = delete;
    M2ModelCache& operator=(const M2ModelCache&) = delete;

    enum class State { Loading, Ready, Failed };
    struct Request {
        State state = State::Loading;
        /// Set when Ready.
        std::shared_ptr<const M2Model> model;
    };
    /// For the frame: the model of `path` when parsed, else the worker that
    /// parses it is asked (once) and the answer is Loading. Failed for a file
    /// that is not there or has no mesh and no emitters, said once where it
    /// failed.
    [[nodiscard]] Request request(const std::string& path);
    /// For a worker: the model of `path`, parsed on this thread when no one
    /// has begun to parse it; null when it cannot be had. It waits only for a
    /// parse already running, never for one still queued: a worker of the
    /// pool that request() queues on would otherwise wait for its own pool.
    [[nodiscard]] std::shared_ptr<const M2Model> load(const std::string& path);

private:
    /// One file's parse, done by whoever claims it first: the worker
    /// request() queued, or a load() that comes before it.
    struct Parse {
        std::promise<std::shared_ptr<const M2Model>> promise;
        std::shared_future<std::shared_ptr<const M2Model>> result = promise.get_future().share();
        std::atomic<bool> claimed{false};
    };
    struct Entry {
        std::shared_ptr<Parse> parse;
        size_t bytes = 0;
        std::list<std::string>::iterator recent;
    };
    [[nodiscard]] static std::string keyOf(const std::string& path);
    [[nodiscard]] std::shared_ptr<const M2Model> parse(const std::string& path) const;
    /// Under the lock: the entry for `key`, made if there is none, with its
    /// parse queued on a worker when `queue`.
    Entry& entryFor(const std::string& key, const std::string& path, bool queue);
    /// Parses `path` into `parse` unless someone claimed it first.
    void run(const std::string& key, const std::string& path, Parse& parse);
    void noteParsed(const std::string& key, const std::shared_ptr<const M2Model>& model);
    void evict();

    AssetManager& assets_;
    const size_t budgetBytes_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> recent_;  // most recently asked for first
    size_t totalBytes_ = 0;
};

} // namespace pipeline
} // namespace wowee
