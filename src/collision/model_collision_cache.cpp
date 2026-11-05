#include "collision/model_collision_cache.hpp"

#include "collision/movement_world.hpp"
#include "foundation/logger.hpp"
#include "pipeline/m2_loader.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace wowee::collision {

namespace {

std::shared_ptr<const M2Collision> build(const std::string& key, const pipeline::M2Model& model) {
    auto collision = std::make_shared<M2Collision>();
    collision->shape = M2Shape::build(key, model.collisionVertices, model.collisionIndices, model.collisionNormals);
    collision->boundsMin = model.boundMin;
    collision->boundsMax = model.boundMax;
    return collision;
}

}  // namespace

struct ModelCollisionCache::State {
    struct Entry {
        std::shared_ptr<const M2Collision> collision;
        std::string unavailable;
        bool loading = false;
    };
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
    std::shared_ptr<const ModelFileSource> source;
    bool noSourceSaid = false;
    std::atomic<uint64_t> revision{0};

    void land(const std::string& key, std::shared_ptr<const M2Collision> collision, std::string unavailable) {
        {
            std::lock_guard lock(mutex);
            auto& entry = entries[key];
            entry.loading = false;
            if (entry.collision) return;  // offered while it loaded
            entry.collision = std::move(collision);
            entry.unavailable = std::move(unavailable);
        }
        ++revision;
    }
};

ModelCollisionCache::ModelCollisionCache() : state_(std::make_shared<State>()) {}

ModelCollisionCache::~ModelCollisionCache() = default;

void ModelCollisionCache::setSource(std::shared_ptr<const ModelFileSource> source) {
    std::lock_guard lock(state_->mutex);
    state_->source = std::move(source);
}

std::shared_ptr<const M2Collision> ModelCollisionCache::offer(const std::string& path,
                                                              const pipeline::M2Model& model) {
    const std::string key = movementModelKey(path);
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->entries.find(key);
        if (found != state_->entries.end() && found->second.collision) return found->second.collision;
    }
    auto collision = build(key, model);
    {
        std::lock_guard lock(state_->mutex);
        auto& entry = state_->entries[key];
        if (entry.collision) return entry.collision;
        entry.collision = collision;
        entry.unavailable.clear();
    }
    ++state_->revision;
    return collision;
}

ModelCollisionCache::Lookup ModelCollisionCache::find(const std::string& path) {
    const std::string key = movementModelKey(path);
    std::shared_ptr<const ModelFileSource> source;
    {
        std::lock_guard lock(state_->mutex);
        auto& entry = state_->entries[key];
        if (entry.collision || !entry.unavailable.empty() || entry.loading) {
            return Lookup{entry.collision, entry.unavailable};
        }
        source = state_->source;
        if (!source) {
            if (!state_->noSourceSaid) {
                state_->noSourceSaid = true;
                LOG_WARNING("Model collision: ", key, " is asked for with no file source - models nobody "
                            "offers stay pending");
            }
            return {};
        }
        entry.loading = true;
    }
    source->runInBackground([state = state_, source, key] {
        const auto bytes = source->read(key);
        std::string why;
        if (!bytes) why = "the file is not in the data";
        else if (bytes->size() < 4 || std::memcmp(bytes->data(), "MD20", 4) != 0) why = "the file is not an M2 model";
        if (why.empty()) {
            state->land(key, build(key, pipeline::M2Loader::load(*bytes)), {});
            return;
        }
        LOG_ERROR("Model collision: ", key, " cannot be read - ", why, "; what places it is not solid");
        state->land(key, nullptr, why);
    });
    return {};
}

uint64_t ModelCollisionCache::revision() const { return state_->revision.load(); }

}  // namespace wowee::collision
