#include "rendering/corpse_bones.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <unordered_set>

namespace wowee {
namespace rendering {

uint32_t CorpseBones::modelFor(M2Renderer* m2, pipeline::AssetManager* assets, const std::string& path) {
    uint32_t modelId = 0;
    if (auto it = modelIds_.find(path); it != modelIds_.end()) {
        modelId = it->second;
        if (modelId == 0) return 0;  // tried, and it is not there
        // Loaded once already, unless a map change has emptied the renderer.
        if (m2->hasModel(modelId)) return modelId;
        // It took its instances with it, and their ids may since have gone
        // to something else - forget them rather than remove a stranger.
        for (auto p = placed_.begin(); p != placed_.end();) {
            p = p->second.modelId == modelId ? placed_.erase(p) : std::next(p);
        }
    } else {
        if (nextModelId_ > LAST_MODEL_ID) return 0;
        modelId = nextModelId_++;
    }

    const auto data = assets ? assets->readFile(path) : std::vector<uint8_t>{};
    pipeline::M2Model model = data.empty() ? pipeline::M2Model{} : pipeline::M2Loader::load(data);
    if (model.vertices.empty()) {
        LOG_WARNING("CorpseBones: ", path, " is missing or empty - those bones will not be drawn");
        modelIds_[path] = 0;
        return 0;
    }
    if (model.name.empty()) model.name = path;
    if (model.version >= 264) {
        const auto skin = assets->readFile(pipeline::skinPathForM2(path));
        if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
    }
    if (!m2->loadModel(model, modelId)) {
        LOG_WARNING("CorpseBones: could not load ", path);
        modelIds_[path] = 0;
        return 0;
    }
    // Bones come and go with players dying nearby; keep the few skeletons
    // resident rather than have the idle-model reaper load them over again.
    m2->setModelPinned(modelId, true);
    modelIds_[path] = modelId;
    LOG_INFO("CorpseBones: loaded ", path, " as model ", modelId);
    return modelId;
}

void CorpseBones::update(M2Renderer* m2, SceneCommands::Models& models, pipeline::AssetManager* assets,
                         const std::vector<Bones>& bones) {
    if (m2 != m2_) {
        // A different renderer holds none of the instances or models kept here.
        placed_.clear();
        modelIds_.clear();
        nextModelId_ = FIRST_MODEL_ID;
        m2_ = m2;
    }
    if (!m2) return;
    if (bones.empty() && placed_.empty()) return;

    std::unordered_set<uint64_t> present;
    present.reserve(bones.size());
    for (const auto& b : bones) {
        present.insert(b.guid);
        const uint32_t modelId = modelFor(m2, assets, b.modelPath);
        if (modelId == 0) continue;
        auto it = placed_.find(b.guid);
        if (it != placed_.end() && it->second.modelId == modelId && models.hasInstance(it->second.instanceId)) {
            continue;  // bones do not move
        }
        if (it != placed_.end() && models.hasInstance(it->second.instanceId)) {
            models.removeInstance(it->second.instanceId);
        }
        // Not deduplicated by position (no instance made through the scene's
        // changes is): two players dying on the same spot leave two sets of
        // bones, and each goes when its own corpse does.
        const uint32_t id = models.createInstance(modelId, b.position, glm::vec3(0.0f, 0.0f, b.yaw), 1.0f);
        if (id == 0) continue;
        // A server object rather than scenery, so it keeps to the range the
        // server shows it at; and something to walk over, not into.
        models.setInstanceIsGameObject(id, true);
        models.setSkipCollision(id, true);
        placed_[b.guid] = Placed{id, modelId};
    }

    for (auto it = placed_.begin(); it != placed_.end();) {
        if (present.count(it->first) == 0) {
            if (models.hasInstance(it->second.instanceId)) models.removeInstance(it->second.instanceId);
            it = placed_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace rendering
} // namespace wowee
