#pragma once

#include "foundation/job_group.hpp"

#include <cstdint>
#include <unordered_map>

namespace wowee::core {

/// Appearance requests belonging to one map's entities. Identity and replacement
/// are the client's rules; JobGroup only fences and cancels background work.
class AssetRequests {
public:
    enum class Channel { Creature, Player, Equipment, GameObject };

    explicit AssetRequests(JobGroup& jobs) : jobs_(jobs) {}
    AssetRequests(const AssetRequests&) = delete;
    AssetRequests& operator=(const AssetRequests&) = delete;

    JobGroup::Token replace(Channel channel, uint64_t guid) {
        auto& token = requests_[{channel, guid}];
        token.cancel();
        token = jobs_.requestToken();
        return token;
    }
    JobGroup::Token request(Channel channel, uint64_t guid) {
        if (auto it = requests_.find({channel, guid}); it != requests_.end()) return it->second;
        return replace(channel, guid);
    }
    void cancel(Channel channel, uint64_t guid) {
        if (auto it = requests_.find({channel, guid}); it != requests_.end()) {
            it->second.cancel();
            requests_.erase(it);
        }
    }

private:
    struct Key {
        Channel channel;
        uint64_t guid;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        size_t operator()(const Key& key) const {
            return std::hash<uint64_t>{}(key.guid) ^ (static_cast<size_t>(key.channel) << 1);
        }
    };
    JobGroup& jobs_;
    std::unordered_map<Key, JobGroup::Token, Hash> requests_;
};

} // namespace wowee::core
