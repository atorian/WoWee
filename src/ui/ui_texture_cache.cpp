#include "ui/ui_texture_cache.hpp"
#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include <chrono>

namespace wowee::ui {
std::string uiTexturePath(const std::string& path) {
    if (path.empty()) return {};
    auto normalized = rendering::normalizeTexturePath(path);
    if (normalized.ends_with(".tga")) normalized.resize(normalized.size() - 4);
    if (!normalized.ends_with(".blp")) normalized += ".blp";
    return normalized;
}
std::string UiTextureCache::key(const std::string& path, bool additive) {
    return uiTexturePath(path) + (additive ? "|add" : "");
}
UiTextureCache::UiTextureCache(Load load, Publish publish, Retire retire)
    : load_(std::move(load)), publish_(std::move(publish)), retire_(std::move(retire)) {}
UiTextureCache::~UiTextureCache() {
    join();
    for (auto& [_, e] : entries_) if (e.descriptor) retire_(e.descriptor);
}
void UiTextureCache::join() {
    for (auto* e : active_) if (e->future.valid()) {
        try { e->prepared = e->future.get(); }
        catch (const std::exception& error) { fail(*e, error.what()); }
    }
}
void UiTextureCache::request(const std::string& path, bool additive, bool visible) {
    if (path.empty()) return;
    auto [it, inserted] = entries_.try_emplace(key(path, additive));
    auto& e = it->second;
    if (inserted) {
        e.path = uiTexturePath(path);
        e.additive = additive;
        waiting_.push_back(&e);
    }
    if (visible && e.state == State::Queued && !e.urgent) {
        e.urgent = true;
        urgent_.push_back(&e);
    }
}
void UiTextureCache::fail(Entry& e, const std::string& reason) {
    if (e.state == State::Failed) return;
    e.state = State::Failed;
    ++failed_;
    LOG_WARNING("Interface texture '", e.path, "': ", reason,
                "; image remains unavailable (no per-frame retry)");
}
void UiTextureCache::pump() {
    using namespace std::chrono_literals;
    for (auto it = active_.begin(); it != active_.end();) {
        Entry& e = **it;
        if (e.state == State::Loading && e.future.wait_for(0s) == std::future_status::ready) {
            try {
                e.prepared = e.future.get();
                if (!e.prepared.uploaded && (e.prepared.width <= 0 || e.prepared.height <= 0))
                    fail(e, e.prepared.failure.empty() ? "read, decode or GPU allocation failed" : e.prepared.failure);
                else e.state = State::Uploading;
            } catch (const std::exception& error) { fail(e, error.what()); }
        }
        if (e.state == State::Uploading) {
            const int done = e.prepared.uploaded ? e.prepared.uploaded->load() : 1;
            if (done < 0) fail(e, "GPU upload failed");
            else if (done > 0 && (e.prepared.width <= 0 || e.prepared.height <= 0)) {
                fail(e, e.prepared.failure.empty() ? "GPU preparation failed" : e.prepared.failure);
                e.prepared.pendingImage.reset(); // the copy has fenced
            } else if (done > 0) {
                try {
                    e.descriptor = publish_(e.prepared);
                    if (!e.descriptor) fail(e, "descriptor publication failed");
                    else { e.state = State::Ready; ++ready_; }
                } catch (const std::exception& error) { fail(e, error.what()); }
            }
        }
        if (e.state == State::Ready || e.state == State::Failed) it = active_.erase(it);
        else ++it;
    }
    // Bounds CPU results and in-flight GPU allocations, not textures/frame.
    // The shared pool bounds execution to two I/O workers.
    constexpr size_t maxOutstanding = 8;
    while (active_.size() < maxOutstanding && (!urgent_.empty() || !waiting_.empty())) {
        auto& queue = urgent_.empty() ? waiting_ : urgent_;
        Entry* e = queue.front(); queue.pop_front();
        if (e->state != State::Queued) continue;
        e->state = State::Loading;
        e->future = core::ThreadPool::ioWorkers().submit("interface texture", [load = load_, path = e->path, add = e->additive] {
            return load(path, add);
        });
        active_.push_back(e);
    }
}
const VkDescriptorSet* UiTextureCache::resident(const std::string& path, bool add) const {
    const auto it = entries_.find(key(path, add));
    if (it == entries_.end()) return nullptr;
    const auto& e = it->second;
    return e.state == State::Ready || e.state == State::Failed ? &e.descriptor : nullptr;
}
bool UiTextureCache::size(const std::string& path, float& w, float& h) const {
    const auto it = entries_.find(key(path, false));
    if (it == entries_.end() || it->second.prepared.width <= 0) return false;
    w = static_cast<float>(it->second.prepared.width);
    h = static_cast<float>(it->second.prepared.height);
    return true;
}
UiTextureCache::State UiTextureCache::state(const std::string& path, bool add) const {
    const auto it = entries_.find(key(path, add));
    return it == entries_.end() ? State::Queued : it->second.state;
}
}
