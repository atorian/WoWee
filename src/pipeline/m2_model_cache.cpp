#include "pipeline/m2_model_cache.hpp"

#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <vector>

namespace wowee {
namespace pipeline {

namespace {

size_t trackBytes(const M2AnimationTrack& track) {
    size_t bytes = 0;
    for (const auto& keys : track.sequences) {
        bytes += keys.timestamps.size() * sizeof(uint32_t) + keys.vec3Values.size() * sizeof(glm::vec3) +
                 keys.quatValues.size() * sizeof(glm::quat) + keys.floatValues.size() * sizeof(float);
    }
    return bytes;
}

/// What the parse holds, near enough to budget by: the mesh and the keys.
size_t bytesOf(const M2Model& model) {
    size_t bytes = sizeof(M2Model) + model.vertices.size() * sizeof(M2Vertex) +
                   model.indices.size() * sizeof(uint16_t);
    for (const auto& bone : model.bones)
        bytes += sizeof(M2Bone) + trackBytes(bone.translation) + trackBytes(bone.rotation) + trackBytes(bone.scale);
    return bytes;
}

} // namespace

M2ModelCache::M2ModelCache(AssetManager& assets, size_t budgetBytes)
    : assets_(assets), budgetBytes_(budgetBytes) {}

M2ModelCache::~M2ModelCache() {
    // A parse still running on a worker notes its result here: it finishes
    // first.
    std::vector<std::shared_future<std::shared_ptr<const M2Model>>> running;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, entry] : entries_) running.push_back(entry.parse);
    }
    for (const auto& parse : running)
        if (parse.valid()) parse.wait();
}

std::string M2ModelCache::keyOf(const std::string& path) {
    std::string key = path;
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

std::shared_ptr<const M2Model> M2ModelCache::parse(const std::string& path) const {
    auto model = std::make_shared<M2Model>();
    if (!loadM2WithSkin(assets_, path, *model)) {
        LOG_WARNING("Unit model ", path, " is missing or has no mesh and no emitters - not drawn");
        return nullptr;
    }
    // Every sequence kept beside it, which is what a unit plays.
    if (const auto bytes = assets_.readFileShared(path)) loadExternalAnimations(assets_, path, *bytes, *model);
    return model;
}

M2ModelCache::Entry& M2ModelCache::entryFor(
        const std::string& key, const std::string& path, bool onWorker,
        std::shared_ptr<std::promise<std::shared_ptr<const M2Model>>>* parseHere) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        recent_.splice(recent_.begin(), recent_, it->second.recent);
        return it->second;
    }
    recent_.push_front(key);
    Entry entry;
    entry.recent = recent_.begin();
    if (onWorker) {
        entry.parse = core::ThreadPool::ioWorkers()
                          .submit("unit model", [this, key, path]() {
                              auto model = parse(path);
                              noteParsed(key, model);
                              return model;
                          })
                          .share();
    } else {
        *parseHere = std::make_shared<std::promise<std::shared_ptr<const M2Model>>>();
        entry.parse = (*parseHere)->get_future().share();
    }
    return entries_.emplace(key, std::move(entry)).first->second;
}

void M2ModelCache::noteParsed(const std::string& key, const std::shared_ptr<const M2Model>& model) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    it->second.bytes = model ? bytesOf(*model) : 0;
    totalBytes_ += it->second.bytes;
    evict();
}

void M2ModelCache::evict() {
    // The least recently asked for first, and never one still parsing.
    auto it = recent_.end();
    while (totalBytes_ > budgetBytes_ && it != recent_.begin()) {
        --it;
        auto entry = entries_.find(*it);
        if (entry->second.parse.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        totalBytes_ -= entry->second.bytes;
        entries_.erase(entry);
        it = recent_.erase(it);
    }
}

M2ModelCache::Request M2ModelCache::request(const std::string& path) {
    const std::string key = keyOf(path);
    std::shared_future<std::shared_ptr<const M2Model>> parse;
    {
        std::lock_guard lock(mutex_);
        parse = entryFor(key, path, true, nullptr).parse;
    }
    if (parse.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {State::Loading, nullptr};
    auto model = parse.get();
    return model ? Request{State::Ready, std::move(model)} : Request{State::Failed, nullptr};
}

std::shared_ptr<const M2Model> M2ModelCache::load(const std::string& path) {
    const std::string key = keyOf(path);
    std::shared_ptr<std::promise<std::shared_ptr<const M2Model>>> parseHere;
    std::shared_future<std::shared_ptr<const M2Model>> parse;
    {
        std::lock_guard lock(mutex_);
        parse = entryFor(key, path, false, &parseHere).parse;
    }
    if (parseHere) {
        auto model = this->parse(path);
        noteParsed(key, model);
        parseHere->set_value(model);
        return model;
    }
    return parse.get();
}

} // namespace pipeline
} // namespace wowee
