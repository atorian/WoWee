#pragma once

#include "collision/m2_shape.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::pipeline { struct M2Model; }

namespace wowee::collision {

/// An M2 model's collision: its authored collision mesh - none for a model
/// that has no collision triangles - and the bounding box its header gives.
struct M2Collision {
    std::shared_ptr<const M2Shape> shape;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

/// Where the cache reads a model file it has to load itself, and on which
/// thread: the client's IO workers, or - in a test - at once.
class ModelFileSource {
public:
    virtual ~ModelFileSource() = default;
    /// The file's bytes, or null when the data does not have it. Called from
    /// a job runInBackground started.
    [[nodiscard]] virtual std::shared_ptr<const std::vector<uint8_t>> read(const std::string& path) const = 0;
    /// Runs `job` away from the frame.
    virtual void runInBackground(std::function<void()> job) const = 0;
};

/// The collision of every M2 model, by path: the one place it is read and
/// built. The renderer offers each model it has parsed anyway; anyone asking
/// for a model nobody has parsed - the movement world, for a doodad the
/// renderer did not draw - has it read here, away from the frame.
///
/// Keyed by movementModelKey. Thread-safe.
class ModelCollisionCache {
public:
    ModelCollisionCache();
    ~ModelCollisionCache();
    ModelCollisionCache(const ModelCollisionCache&) = delete;
    ModelCollisionCache& operator=(const ModelCollisionCache&) = delete;

    /// Where a model nobody offered is read from. None: such a model stays
    /// pending, and that is said once.
    void setSource(std::shared_ptr<const ModelFileSource> source);

    /// A model someone has parsed: its collision, built here on the first
    /// offer and shared after.
    std::shared_ptr<const M2Collision> offer(const std::string& path, const pipeline::M2Model& model);

    struct Lookup {
        /// Built: the collision. Null while it loads or when it cannot.
        std::shared_ptr<const M2Collision> collision;
        /// Why it cannot be had; empty while it loads or once it is built.
        std::string unavailable;
    };
    /// The model's collision, or - the first time it is asked for and nobody
    /// has offered it - the start of reading it.
    [[nodiscard]] Lookup find(const std::string& path);

    /// Counts every model that became built or unavailable: who waits for
    /// models looks again when it changes.
    [[nodiscard]] uint64_t revision() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace wowee::collision
