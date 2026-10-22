#pragma once

#include "rendering/texture_cache.hpp"
#include <vulkan/vulkan.h>
#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>

namespace wowee::ui {

// File spelling is not resource identity: addons use case, slashes, .tga and
// omitted extensions interchangeably.
std::string uiTexturePath(const std::string& path);

struct PreparedUiTexture {
    rendering::TextureRef image;
    int width = 0, height = 0;
    // null for a CPU-only backend; otherwise 0 pending, 1 fenced, -1 failed.
    std::shared_ptr<std::atomic<int>> uploaded;
    // Held outside the shared cache until this upload has fenced. No other
    // cache consumer can discover an image still being copied.
    std::shared_ptr<rendering::VkTexture> pendingImage;
    rendering::TextureKey key;
    std::string failure;
};

// Producer-thread requests, bounded work on the shared I/O pool, and a
// nonblocking producer-thread publish. Workers never touch widgets or Lua.
// The backend owns GPU operations; this queue also runs with a fake backend.
class UiTextureCache {
public:
    enum class State { Queued, Loading, Uploading, Ready, Failed };
    using Load = std::function<PreparedUiTexture(const std::string&, bool)>;
    using Publish = std::function<VkDescriptorSet(PreparedUiTexture&)>;
    using Retire = std::function<void(VkDescriptorSet&)>;
    UiTextureCache(Load load, Publish publish, Retire retire);
    ~UiTextureCache();
    void request(const std::string& path, bool additive = false, bool visible = false);
    void pump();
    // Join only at teardown/dataset changes, never in a frame consumer.
    void join();
    const VkDescriptorSet* resident(const std::string& path, bool additive = false) const;
    bool size(const std::string& path, float& width, float& height) const;
    State state(const std::string& path, bool additive = false) const;
    size_t pending() const { return entries_.size() - ready_ - failed_; }
    size_t ready() const { return ready_; }
    size_t failed() const { return failed_; }
private:
    struct Entry {
        std::string path;
        bool additive = false, urgent = false;
        State state = State::Queued;
        PreparedUiTexture prepared;
        std::future<PreparedUiTexture> future;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };
    static std::string key(const std::string& path, bool additive);
    void fail(Entry& e, const std::string& reason);
    Load load_;
    Publish publish_;
    Retire retire_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<Entry*> waiting_, urgent_;
    std::vector<Entry*> active_;
    size_t ready_ = 0, failed_ = 0;
};
}
