#pragma once

#include <cstdint>
#include <exception>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace wowee::pipeline {

/// Bounded, immutable CPU preparation shared by synchronous worker requests.
/// One caller computes a key outside the lock; concurrent callers wait for that
/// same result. Different keys run independently. Do not request the same key
/// recursively from its loader. Owners must join their workers before destruction.
/// Eviction releases cache ownership only: queued uploads keep their own pins.
/// Failed results may be represented by an invalid Value and are cached too;
/// exceptions wake every waiter and are not retained.
template<class Value>
class PreparationCache {
public:
    using Result = std::shared_ptr<const Value>;
    struct Stats {
        size_t entries = 0, bytes = 0, budgetBytes = 0, active = 0;
        uint64_t hits = 0, misses = 0, joined = 0, evicted = 0, oversized = 0;
    };

    explicit PreparationCache(size_t budgetBytes, size_t maxEntries = 4096)
        : budget_(budgetBytes), maxEntries_(maxEntries) {}

    template<class Loader, class Cost>
    Result get(const std::string& key, Loader&& loader, Cost&& cost) {
        std::shared_ptr<Flight> flight;
        uint64_t generation = 0;
        bool produce = false;
        {
            std::lock_guard lock(mutex_);
            if (auto hit = ready_.find(key); hit != ready_.end()) {
                lru_.splice(lru_.begin(), lru_, hit->second);
                ++stats_.hits;
                return hit->second->value;
            }
            if (auto pending = active_.find(key); pending != active_.end()) {
                flight = pending->second;
                ++stats_.joined;
            } else {
                flight = std::make_shared<Flight>();
                active_.emplace(key, flight);
                generation = generation_;
                ++stats_.misses;
                ++stats_.active;
                produce = true;
            }
        }
        if (!produce) return flight->result.get();
        try {
            Result value;
            if constexpr (std::is_same_v<std::invoke_result_t<Loader, uint64_t>, Result>)
                value = loader(generation);
            else
                value = std::make_shared<const Value>(loader(generation));
            const size_t bytes = sizeof(Entry) + sizeof(Value) + key.size() + cost(*value);
            {
                std::lock_guard lock(mutex_);
                if (generation == generation_) {
                    if (bytes <= budget_ && maxEntries_) {
                        while (!lru_.empty() &&
                               (bytes > budget_ - stats_.bytes || lru_.size() >= maxEntries_))
                            evictOldest();
                        lru_.push_front({key, value, bytes});
                        try { ready_.emplace(key, lru_.begin()); }
                        catch (...) { lru_.pop_front(); throw; }
                        stats_.bytes += bytes;
                    } else {
                        ++stats_.oversized;
                    }
                }
                finish(key, flight);
            }
            flight->promise.set_value(value);
            return value;
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                finish(key, flight);
            }
            flight->promise.set_exception(std::current_exception());
            throw;
        }
    }

    /// Old readers finish with their pinned result; new requests never join or
    /// cache work from before this boundary, even when the old producer is slow.
    void clear() {
        std::lock_guard lock(mutex_);
        ++generation_;
        ready_.clear();
        lru_.clear();
        active_.clear();
        stats_.bytes = 0;
    }

    void setBudget(size_t bytes) {
        std::lock_guard lock(mutex_);
        budget_ = bytes;
        while (!lru_.empty() && stats_.bytes > budget_) evictOldest();
    }

    Stats stats() const {
        std::lock_guard lock(mutex_);
        Stats result = stats_;
        result.entries = lru_.size();
        result.budgetBytes = budget_;
        return result;
    }

private:
    struct Entry { std::string key; Result value; size_t bytes; };
    struct Flight {
        std::promise<Result> promise;
        std::shared_future<Result> result = promise.get_future().share();
        bool finished = false;
    };
    using Lru = std::list<Entry>;
    mutable std::mutex mutex_;
    Lru lru_;
    std::unordered_map<std::string, typename Lru::iterator> ready_;
    std::unordered_map<std::string, std::shared_ptr<Flight>> active_;
    size_t budget_, maxEntries_;
    uint64_t generation_ = 1;
    Stats stats_;

    void finish(const std::string& key, const std::shared_ptr<Flight>& flight) {
        if (flight->finished) return;
        flight->finished = true;
        --stats_.active;
        auto entry = active_.find(key);
        if (entry != active_.end() && entry->second == flight) active_.erase(entry);
    }
    void evictOldest() {
        auto& oldest = lru_.back();
        stats_.bytes -= oldest.bytes;
        ready_.erase(oldest.key);
        lru_.pop_back();
        ++stats_.evicted;
    }
};

} // namespace wowee::pipeline
