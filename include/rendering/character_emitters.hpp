#pragma once

#include <cstdint>
#include <unordered_map>
#include <memory>

namespace wowee {
namespace rendering {

class CharacterRenderer;
class M2Renderer;

/// The particle and ribbon emitters of what CharacterRenderer draws.
///
/// CharacterRenderer draws creatures, mounts, players and the weapons they
/// carry, and draws meshes only. M2Renderer runs and draws emitters. This
/// gives every CharacterRenderer instance whose model has emitters a set of
/// them in M2Renderer, posed each frame on that instance's skeleton - the
/// riding phoenix's wing trails, a fire elemental's flames, an enchant's
/// sparks - and takes it away with the instance.
///
/// update() runs once both renderers have finished the frame's animation, so
/// the emitters follow this frame's skeleton rather than the last one's.
class CharacterEmitters {
    struct InstanceState;
public:
    /// The owning scene holds this lease. Geometry/texture caches stay here;
    /// instances, collision indexes and incomplete uploads die with the lease.
    class InstanceSession {
    public:
        explicit InstanceSession(CharacterEmitters& renderer);
        ~InstanceSession();
        InstanceSession(const InstanceSession&) = delete;
        InstanceSession& operator=(const InstanceSession&) = delete;
    private:
        CharacterEmitters& renderer_;
        std::unique_ptr<InstanceState> state_;
    };
    CharacterEmitters(CharacterRenderer& characters, M2Renderer& m2);
    ~CharacterEmitters();
    CharacterEmitters(const CharacterEmitters&) = delete;
    CharacterEmitters& operator=(const CharacterEmitters&) = delete;

    void update(float deltaTime);
    void clearInstances();

private:
    struct Model {
        uint32_t handle = 0;  // M2Renderer posed-emitter model; 0 while not loaded
        uint32_t users = 0;
        /// It did not load; not asked for again, as it would fail the same
        /// way every frame.
        bool failed = false;
    };
    struct Binding {
        uint64_t modelSerial = 0;
        uint32_t emitters = 0;  // M2Renderer posed emitters
        uint64_t seenFrame = 0;
    };

    void release(Binding& binding);

    CharacterRenderer& characters_;
    M2Renderer& m2_;
    /// Per CharacterRenderer model load (EmitterSource::modelSerial).
    std::unordered_map<uint64_t, Model> models_;
    /// Per CharacterRenderer instance.
    uint64_t frame_ = 0;
    struct InstanceState {
        std::unordered_map<uint32_t, Binding> bindings_;
    };
    InstanceState defaultInstances_;
    InstanceState* scene_ = &defaultInstances_;

};

} // namespace rendering
} // namespace wowee
