#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace wowee {
namespace rendering {

/// Calls on a target, kept until the target is handed over and then made in
/// the order they were asked for. What SceneCommands queues the scene's
/// changes in; apart from it so the order and the dropping can be tested
/// without a renderer.
template <class Target>
class DeferredCalls {
public:
    using Call = std::function<void(Target&)>;

    void post(Call call) { calls_.push_back(std::move(call)); }

    /// Makes every call posted, oldest first - those a call posts included,
    /// after the rest. Returns how many were made.
    size_t apply(Target& target) {
        size_t made = 0;
        // A call may post more: take the list, and go round until it stays empty.
        while (!calls_.empty()) {
            std::vector<Call> now;
            now.swap(calls_);
            for (Call& call : now) {
                call(target);
                ++made;
            }
        }
        return made;
    }

    /// Drops every call posted: the target they were meant for has gone.
    /// Returns how many there were.
    size_t discard() {
        const size_t dropped = calls_.size();
        calls_.clear();
        return dropped;
    }

    [[nodiscard]] size_t size() const { return calls_.size(); }
    [[nodiscard]] bool empty() const { return calls_.empty(); }

private:
    std::vector<Call> calls_;
};

} // namespace rendering
} // namespace wowee
