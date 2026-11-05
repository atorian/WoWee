#pragma once

#include "rendering/texture_cache.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::rendering {

/// Producer-thread dependencies of material lists on texture bindings. A
/// unique binding is polled once, however many lists share it. Dirty lists
/// stay dirty while hidden; replacing a rebuilt list acknowledges its reads.
/// References keep the images named by the old material rows alive until the
/// list is replaced or removed.
class TextureDependencies {
public:
    void replace(uint32_t list, std::span<const TextureRef> textures) {
        remove(list);
        auto& reads = lists_[list];
        for (const auto& texture : textures) {
            if (!texture) continue;
            auto [it, added] = bindings_.try_emplace(texture);
            const uint64_t revision = texture.revision();
            if (!added && it->second.revision != revision)
                dirty_.insert(it->second.lists.begin(), it->second.lists.end());
            it->second.revision = revision;
            if (it->second.lists.insert(list).second) reads.push_back(texture);
        }
    }

    void remove(uint32_t list) {
        if (auto found = lists_.find(list); found != lists_.end()) {
            for (const auto& texture : found->second) {
                auto at = bindings_.find(texture);
                at->second.lists.erase(list);
                if (at->second.lists.empty()) bindings_.erase(at);
            }
            lists_.erase(found);
        }
        dirty_.erase(list);
    }

    void poll() {
        for (auto& [texture, binding] : bindings_) {
            const uint64_t revision = texture.revision();
            if (binding.revision == revision) continue;
            binding.revision = revision;
            dirty_.insert(binding.lists.begin(), binding.lists.end());
        }
    }

    [[nodiscard]] bool changed(uint32_t list) const { return dirty_.contains(list); }

    void clear() {
        lists_.clear();
        bindings_.clear();
        dirty_.clear();
    }

private:
    struct Binding {
        uint64_t revision = 0;
        std::unordered_set<uint32_t> lists;
    };
    std::unordered_map<TextureRef, Binding, TextureRef::Hash> bindings_;
    std::unordered_map<uint32_t, std::vector<TextureRef>> lists_;
    std::unordered_set<uint32_t> dirty_;
};

} // namespace wowee::rendering
