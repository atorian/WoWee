#pragma once

#include "rendering/scene_commands.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;

/// What is left of a player once the body has gone: a corpse object the
/// server has marked as bones, which the original client draws as the race's
/// skeleton lying on the ground -
/// World\Generic\PassiveDoodads\DeathSkeletons\<Race><Gender>DeathSkeleton.m2.
/// Nothing else of the owner is shown, and the server has cleared the gear.
class CorpseBones {
public:
    struct Bones {
        uint64_t guid = 0;
        std::string modelPath;
        glm::vec3 position{0.0f};  // render coordinates
        float yaw = 0.0f;          // render yaw, radians
    };

    /// Once a frame with every set of bones in view now. Lays new ones down,
    /// and picks up the ones that have gone. The skeletons load in `m2`; the
    /// bones come and go through `models`.
    void update(M2Renderer* m2, SceneCommands::Models& models, pipeline::AssetManager* assets,
                const std::vector<Bones>& bones);

private:
    /// Clear of the level-up effect (999900) and the loot sparkle (999901).
    /// There are twenty-odd skeletons, one per race and gender.
    static constexpr uint32_t FIRST_MODEL_ID = 999910;
    static constexpr uint32_t LAST_MODEL_ID = 999949;

    /// The model id a skeleton is loaded under, loading it if needed; 0 if
    /// it cannot be had.
    uint32_t modelFor(M2Renderer* m2, pipeline::AssetManager* assets, const std::string& path);

    struct Placed {
        uint32_t instanceId = 0;
        uint32_t modelId = 0;
    };

    M2Renderer* m2_ = nullptr;
    std::unordered_map<std::string, uint32_t> modelIds_;  // path -> model id, 0 = failed
    uint32_t nextModelId_ = FIRST_MODEL_ID;
    std::unordered_map<uint64_t, Placed> placed_;         // corpse guid -> instance
};

} // namespace rendering
} // namespace wowee
