#include "rendering/normal_map_builder.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <exception>

namespace wowee {
namespace rendering {

NormalMapBuilder::NormalMapBuilder(size_t workers, size_t capacity, Derive derive)
    : capacity_(std::max<size_t>(capacity, 1)), derive_(std::move(derive)) {
    workers_.reserve(workers);
    for (size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { work(); });
}

NormalMapBuilder::~NormalMapBuilder() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queued_.clear();
    }
    wake_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

NormalMapBuilder::Submitted NormalMapBuilder::submit(Request request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (present_.count(request.key) != 0) {
            for (auto& queued : queued_) {
                if (queued.key == request.key) queued.priority = request.priority;
            }
            return Submitted::AlreadyThere;
        }
        if (queued_.size() >= capacity_) {
            auto farthest = std::max_element(queued_.begin(), queued_.end(),
                                             [](const Request& a, const Request& b) {
                                                 return a.priority < b.priority;
                                             });
            if (farthest == queued_.end() || farthest->priority <= request.priority) {
                ++refused_;
                return Submitted::Refused;
            }
            // The farthest gives its place up; its owner asks again when
            // there is room.
            present_.erase(farthest->key);
            *farthest = std::move(request);
            present_.insert(farthest->key);
            ++displaced_;
        } else {
            present_.insert(request.key);
            queued_.push_back(std::move(request));
        }
    }
    wake_.notify_one();
    return Submitted::Queued;
}

bool NormalMapBuilder::contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return present_.count(key) != 0;
}

std::vector<NormalMapBuilder::Result> NormalMapBuilder::takeFinished(size_t max) {
    std::vector<Result> out;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!finished_.empty() && out.size() < max) {
        present_.erase(finished_.front().key);
        out.push_back(std::move(finished_.front()));
        finished_.pop_front();
    }
    return out;
}

NormalMapBuilder::Stats NormalMapBuilder::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {.queued = queued_.size(),
            .running = running_,
            .finished = finished_.size(),
            .displaced = displaced_,
            .refused = refused_};
}

void NormalMapBuilder::work() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queued_.empty(); });
            if (stopping_) return;
            auto nearest = std::min_element(queued_.begin(), queued_.end(),
                                            [](const Request& a, const Request& b) {
                                                return a.priority < b.priority;
                                            });
            request = std::move(*nearest);
            *nearest = std::move(queued_.back());
            queued_.pop_back();
            ++running_;
        }

        Result result;
        result.key = request.key;
        std::string failure;
        try {
            std::optional<Pixels> source = request.source ? request.source() : std::nullopt;
            if (!source || source->rgba.empty() || source->width == 0 || source->height == 0) {
                failure = "its texture could not be read";
            } else {
                result.map = derive_(std::move(source->rgba), source->width, source->height);
                if (!result.map.valid()) failure = "nothing was derived from it";
            }
        } catch (const std::exception& e) {
            // bad_alloc among them: the map is skipped, the worker lives on.
            failure = e.what();
            result.map = {};
        }
        if (!failure.empty()) {
            result.failed = true;
            LOG_WARNING("Normal map for '", result.key, "' not built: ", failure,
                        " - drawn without one");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
            if (stopping_) return;
            finished_.push_back(std::move(result));
        }
    }
}

} // namespace rendering
} // namespace wowee
