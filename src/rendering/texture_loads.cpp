#include "rendering/texture_loads.hpp"
#include "rendering/vk_texture.hpp"

#include "foundation/logger.hpp"
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
    return requestLocked(key, out, make);
}

TextureLoads::State TextureLoads::requestLocked(const TextureKey& key, TextureRef& out,
                                                const std::function<Make()>& make) {
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
        } else {
            return State::Failed;
        }
    }
    if (stopping_) return State::Failed;
    Load& load = loads_[key];
    load.state = State::Loading;
    queue_.push_back({key, make()});
    queued_.notify_one();
    return State::Loading;
}

TextureLoads::State TextureLoads::requestStored(const std::string& key, TextureRef& out) {
    return request({key, TextureForm::Stored, 0}, out, [&]() -> Make {
        return makeStored({key, TextureForm::Stored, 0});
    });
}

TextureLoads::Make TextureLoads::makeStored(const TextureKey& key) {
    return [this, key]() -> std::vector<PreparedTexture> {
        // Blocks as the file keeps them: these are only ever sampled.
        pipeline::BLPImage file = assets_.loadTexture(key.path, true);
        if (!file.isValid()) {
            static core::LogBudget unread(64, "stored textures that could not be read");
            if (unread.take())
                LOG_WARNING("Stored texture '", key.path, "' could not be read; what asks for it draws without it, "
                            "the failed request is not retried during this session");
            return {};
        }
        // Whether the texture actually uses alpha, and whether that alpha
        // is a silhouette, read from the blocks once. Says itself what it
        // refused.
        file.analyzeAlpha();
        return {prepareStoredTexture(context_, key.path, file)};
    };
}

TextureRef TextureLoads::reference(const TextureKey& key, TextureRef fallback,
                                   const std::function<Make()>& make) {
    TextureRef resident;
    std::lock_guard lock(mutex_);
    const State state = requestLocked(key, resident, make);
    if (state == State::Resident) return resident;
    Load& load = loads_[key];
    if (!load.reference) {
        load.reference = context_.textures().defer(key, {});
        if (state == State::Failed) {
            load.state = State::Failed;
            context_.textures().complete(load.reference, {});
        }
    }
    return load.reference.withFallback(std::move(fallback));
}

TextureRef TextureLoads::stored(const std::string& path, TextureRef fallback) {
    const TextureKey key{normalizeTexturePath(path), TextureForm::Stored, 0};
    return reference(key, std::move(fallback), [this, key]() -> Make {
        return makeStored(key);
    });
}

TextureRef TextureLoads::interfaceTexture(const std::string& path, InterfaceAlpha alpha) {
    const auto source = pipeline::AssetManager::interfaceTexturePath(path);
    const TextureKey key{"interface-generation/" + std::to_string(interfaceGeneration_.load()) + "/" + source,
                         TextureForm::Interface, static_cast<uint8_t>(alpha)};
    return reference(key, {}, [this, source, key, alpha]() -> Make {
        return [this, source, key, alpha]() -> std::vector<PreparedTexture> {
            auto image = assets_.loadInterfaceTexture(source, alpha == InterfaceAlpha::Original);
            if (!image.isValid()) {
                LOG_WARNING("Interface texture '", source, "' could not be read; image is not drawn");
                return {};
            }
            if (image.isBlockCompressed()) image.mipmaps.resize(1);
            bool allOpaque = true;
            if (alpha == InterfaceAlpha::Highlight) for (size_t i = 3; i < image.data.size(); i += 4)
                if (image.data[i] != 255) { allOpaque = false; break; }
            if (alpha == InterfaceAlpha::Additive || (alpha == InterfaceAlpha::Highlight && allOpaque))
                for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
                    const auto light = std::max({image.data[i], image.data[i + 1], image.data[i + 2]});
                    image.data[i + 3] = static_cast<uint8_t>((image.data[i + 3] * light) / 255);
                }
            auto texture = std::make_shared<VkTexture>();
            std::vector<PreparedTexture> made;
            bool uploaded = image.isBlockCompressed() ? texture->uploadBLP(context_, image)
                : texture->upload(context_, image.data.data(), image.width, image.height,
                                  VK_FORMAT_R8G8B8A8_UNORM, false);
            if (!uploaded && image.isBlockCompressed()) {
                LOG_WARNING("Interface texture '", source,
                            "' block upload failed; trying its decoded pixels");
                // Retain a failed image too, in case it recorded any copies.
                made.push_back({.key = key, .texture = std::move(texture), .publish = false});
                texture = std::make_shared<VkTexture>();
                image = assets_.loadInterfaceTexture(source, false);
                uploaded = image.isValid() && texture->upload(context_, image.data.data(), image.width,
                    image.height, VK_FORMAT_R8G8B8A8_UNORM, false);
            }
            if (!uploaded || !texture->createSampler(context_.getDevice(), VK_FILTER_LINEAR,
                    VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f)) {
                LOG_WARNING("Interface texture '", source, "' GPU preparation failed; image is not drawn");
                made.push_back({.key = key, .texture = std::move(texture), .publish = false});
                return made;
            }
            const auto bytes = texture->allocatedBytes();
            made.insert(made.begin(), {.key = key, .texture = std::move(texture), .bytes = bytes});
            return made;
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
            ++working_;
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
            try {
                sent.made.push_back(job.make());
            } catch (const std::exception& error) {
                LOG_WARNING("Texture '", job.key.path, "' preparation failed: ", error.what(),
                            "; drawn with its fallback");
                sent.made.emplace_back();
            }
        }
        sent.completion = std::make_shared<std::atomic<int>>(0);
        context_.endUploadBatch(sent.completion);
        std::lock_guard lock(mutex_);
        sent_.push_back(std::move(sent));
        --working_;
        idle_.notify_all();
    }
}

void TextureLoads::drain() {
    {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && working_ == 0; });
    }
    context_.waitAllUploads();
    publish();
}

void TextureLoads::resetDataset() {
    drain();
    std::lock_guard lock(mutex_);
    loads_.clear();
    context_.textures().invalidateNamed();
    ++interfaceGeneration_;
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
                if (!texture.texture || !texture.publish) continue;
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
    auto it = loads_.find(key);
    if (it != loads_.end() && it->second.reference)
        context_.textures().complete(it->second.reference, made);
    ++revisions_[static_cast<size_t>(key.form)];
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
        load.reference.reset();
    } else {
        load.state = State::Failed;
    }
}

}  // namespace wowee::rendering
