#include "rendering/texture_loads.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"

#include <algorithm>

namespace wowee::rendering {

namespace {

/// The most jobs one upload batch carries: enough that a city's textures go
/// up in a few dozen submits, few enough that the first of them is not kept
/// waiting long for the last to be read.
constexpr size_t kJobsPerBatch = 32;

size_t workerCount() {
    // Reading a file is decompressing it: half the cores, at least two.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp<size_t>(cores / 2, 2, 6);
}

}  // namespace

TextureLoads::TextureLoads(VkContext& context, pipeline::AssetManager& assets)
    : context_(context), assets_(assets) {
    const size_t count = workerCount();
    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) workers_.emplace_back([this] { work(); });
}

TextureLoads::~TextureLoads() {
    {
        // Those not begun yet are dropped.
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    queued_.notify_all();
    for (auto& worker : workers_) worker.join();
    // The batches sent copy into the textures they hold, which go with them.
    context_.waitAllUploads();
    sent_.clear();
}

TextureLoads::State TextureLoads::request(const TextureKey& key, TextureRef& out,
                                          const std::function<Make()>& make) {
    std::lock_guard lock(mutex_);
    TextureCache& cache = context_.textures();
    if (TextureRef resident = cache.find(key)) {
        out = std::move(resident);
        return State::Resident;
    }
    if (auto it = loads_.find(key); it != loads_.end()) {
        Load& load = it->second;
        if (load.state == State::Loading) return State::Loading;
        if (load.state == State::Resident) {
            // Made under another name than it was asked for by (a building's
            // texture found under its second name): the record names it.
            // Evicted since, it is made again.
            if (TextureRef resident = cache.find(load.madeAs)) {
                out = std::move(resident);
                return State::Resident;
            }
        } else if (++load.requestsSinceFailure < kRetryAfterRequests) {
            // Failed: said once by the worker, and left alone for a while.
            return State::Failed;
        }
    }
    if (stopping_) return State::Failed;
    loads_[key] = Load{};
    queue_.push_back({key, make()});
    queued_.notify_one();
    return State::Loading;
}

TextureLoads::State TextureLoads::requestStored(const std::string& key, TextureRef& out) {
    return request({key, TextureForm::Stored, 0}, out, [&]() -> Make {
        return [this, key]() -> std::vector<PreparedTexture> {
            // Blocks as the file keeps them: these are only ever sampled.
            pipeline::BLPImage file = assets_.loadTexture(key, true);
            if (!file.isValid()) {
                static core::LogBudget unread(64, "stored textures that could not be read");
                if (unread.take())
                    LOG_WARNING("Stored texture '", key, "' could not be read; what asks for it draws without it, "
                                "and asks again after ", kRetryAfterRequests, " more requests");
                return {};
            }
            // Whether the texture actually uses alpha, and whether that alpha
            // is a silhouette, read from the blocks once. Says itself what it
            // refused.
            file.analyzeAlpha();
            return {prepareStoredTexture(context_, key, file)};
        };
    });
}

void TextureLoads::work() {
    for (;;) {
        std::vector<Job> jobs;
        {
            std::unique_lock lock(mutex_);
            queued_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            while (!queue_.empty() && jobs.size() < kJobsPerBatch) {
                jobs.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
        // This thread's command buffers of batches that have run go back to
        // its pool (only its owner may free them).
        context_.pollUploadBatches();
        // Every copy of the batch recorded and sent at once; nothing waits
        // for it. publish() puts the textures in the cache once it has run.
        Sent sent;
        sent.keys.reserve(jobs.size());
        sent.made.reserve(jobs.size());
        context_.beginUploadBatch();
        for (Job& job : jobs) {
            sent.keys.push_back(job.key);
            sent.made.push_back(job.make());
        }
        sent.completion = std::make_shared<std::atomic<int>>(0);
        context_.endUploadBatch(sent.completion);
        std::lock_guard lock(mutex_);
        sent_.push_back(std::move(sent));
    }
}

void TextureLoads::publish() {
    // Each sent batch's fence read: a batch that has run says so here.
    context_.pollUploadBatches();
    std::vector<Sent> done;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < sent_.size();) {
            if (sent_[i].completion->load() == 0) {
                ++i;
                continue;
            }
            done.push_back(std::move(sent_[i]));
            sent_[i] = std::move(sent_.back());
            sent_.pop_back();
        }
    }
    if (done.empty()) return;
    TextureCache& cache = context_.textures();
    std::vector<std::pair<TextureKey, TextureRef>> answers;
    for (Sent& batch : done) {
        const bool ran = batch.completion->load() > 0;
        if (!ran) {
            LOG_WARNING("Texture loads: a batch of ", batch.keys.size(),
                        " textures did not run on the GPU; what asked for them draws without them");
        }
        for (size_t i = 0; i < batch.keys.size(); ++i) {
            TextureRef answer;
            for (size_t t = 0; ran && t < batch.made[i].size(); ++t) {
                PreparedTexture& texture = batch.made[i][t];
                if (!texture.texture) continue;
                TextureRef inserted = cache.insert(texture.key, std::move(texture.texture), texture.bytes, texture.info);
                if (t == 0) answer = std::move(inserted);
            }
            answers.emplace_back(batch.keys[i], std::move(answer));
        }
    }
    std::lock_guard lock(mutex_);
    for (const auto& [key, made] : answers) finish(key, made);
}

void TextureLoads::finish(const TextureKey& key, const TextureRef& made) {
    if (made && made.key() == key) {
        // In the cache under the name it is asked for by: nothing to keep.
        // Nothing holds it either, so a load nobody follows up - a tile left
        // before it was finished - pins nothing.
        loads_.erase(key);
        return;
    }
    Load& load = loads_[key];
    if (made) {
        load.state = State::Resident;
        load.madeAs = made.key();
    } else {
        load.state = State::Failed;
        load.requestsSinceFailure = 0;
    }
}

}  // namespace wowee::rendering
