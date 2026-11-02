#pragma once

// Runs a frame's M2 particles and ribbons on the GPU (m2_particles.comp,
// m2_ribbons.comp) - the only place they are - for a renderer that keeps
// its models' emitters in a SkeletonPool and their particles and edges in
// a ParticlePool: the renderer lists at extraction the emitters that run
// or are seen (a ParticleFrame), and the pass, on the render thread after
// the frame's skeletons were posed, runs them, lays out the seen ones'
// quads and strips and a draw command for each, and copies back what the
// CPU reads of them: how many particles or edges are alive and how far the
// particles reach. The draws read the quads as m2_particle.vert's
// instances and the strips as m2_ribbon.vert's vertices, by the commands.
//
// What the CPU reads comes back two frames late, as the skeletons' bones.

#include "rendering/interior_light.hpp"
#include "rendering/local_lights.hpp"
#include "rendering/vk_utils.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

/// One emitter of one instance for the frame, as m2_particles.comp reads it
/// (Row there, std430).
struct ParticleRowGPU {
    /// Bit 0: it emits. Bit 1: it is seen - its quads are laid out. Bit 2:
    /// fireflies, which drift standing still. Bit 3: lit. Bit 4: it stands on
    /// a skeleton of this frame's posed bones; bit 5: on a creature's.
    static constexpr uint32_t kEmit = 1u;
    static constexpr uint32_t kInView = 2u;
    static constexpr uint32_t kFirefly = 4u;
    static constexpr uint32_t kLit = 8u;
    static constexpr uint32_t kM2Skeleton = 16u;
    static constexpr uint32_t kCharacterSkeleton = 32u;

    uint32_t blobPage = 0, blobFirst = 0, emitter = 0, flags = 0;
    uint32_t poolPage = 0, poolFirst = 0, capacity = 0, tag = 0;
    int32_t sequence = 0;
    float time = 0.0f, globalTime = 0.0f, dt = 0.0f;
    uint32_t request = 0, boneBase = 0, boneCount = 0, seed = 0;
    uint32_t frameSerial = 0, pad0 = 0, pad1 = 0, pad2 = 0;
    /// xyz: its instance's cull centre, which its reach is measured from;
    /// w: its instance's fade.
    glm::vec4 cullCentre{0.0f, 0.0f, 0.0f, 1.0f};
    /// Its model matrix, where no skeleton was posed for it.
    glm::mat4 placement{1.0f};
    InteriorLightGPU light;
    LocalLightSet localLights = kNoLocalLights;
};
static_assert(sizeof(ParticleRowGPU) == 208, "ParticleRowGPU must match Row in m2_particles.comp (std430)");

/// What one frame runs: its rows - the particle emitters', then from
/// `ribbonFirst` the ribbons' - whose each one is, and the pools' pages as
/// they were.
struct ParticleFrame {
    /// Whose a row is, as the renderer names it: an instance's emitter or
    /// ribbon, or a posed set's.
    struct Owner {
        uint32_t id = 0;
        uint16_t emitter = 0;
        bool posed = false;
        bool ribbon = false;
    };
    std::vector<ParticleRowGPU> rows;
    uint32_t ribbonFirst = 0;
    std::vector<Owner> owners;
    std::vector<::VkBuffer> blobPages, poolPages;
    uint32_t blobVersion = 0, poolVersion = 0;
    glm::vec3 eye{0.0f};
    float density = 1.0f;
    void clear() noexcept;
};

class M2ParticlePass {
public:
    /// A row's emitter as the GPU last left it: its live particles or
    /// edges, and how far the particles reach.
    struct Read {
        ParticleFrame::Owner owner;
        uint32_t live = 0;
        float reach = 0.0f;
    };

    M2ParticlePass() = default;
    M2ParticlePass(const M2ParticlePass&) = delete;
    M2ParticlePass& operator=(const M2ParticlePass&) = delete;

    /// The pipeline and each frame slot's set; `bonesLayout` is the posed
    /// bones' set (SkeletonPass::drawSetLayout). False, said, when one could
    /// not be made: no particle runs or is drawn.
    bool initialize(VkContext& context, VkDescriptorSetLayout bonesLayout, const char* owner);
    /// Everything let go after the frames in flight.
    void destroy();

    /// Takes what the slot's last frame copied back, and runs `frame` into
    /// the slot's quads and draw commands. Outside a render pass, after the
    /// frame's skeletons were posed. `bones` is this renderer's posed bones,
    /// `characterBones` the creatures' (any valid set where there are none)
    /// and `charactersPosed` whether that holds this frame's. False, said,
    /// when there was no memory: nothing of the frame is drawn then.
    bool record(VkCommandBuffer cmd, uint32_t slot, const ParticleFrame& frame, VkDescriptorSet bones,
                VkDescriptorSet characterBones, bool charactersPosed);
    /// The rows read back since the last call.
    [[nodiscard]] std::vector<Read> takeRead();
    /// The quads the latest frame read back laid out.
    [[nodiscard]] uint32_t quadsLaidOut() const { return quadsLaidOut_.load(std::memory_order_relaxed); }

    /// The slot's quads (m2_particle.vert's instance buffer), ribbon
    /// vertices (m2_ribbon.vert's) and draw commands, a VkDrawIndirectCommand
    /// by row, as its last record() left them.
    [[nodiscard]] ::VkBuffer quads(uint32_t slot) const { return slots_[slot].quads.buffer; }
    [[nodiscard]] ::VkBuffer ribbonVertices(uint32_t slot) const { return slots_[slot].ribbonVertices.buffer; }
    [[nodiscard]] ::VkBuffer draws(uint32_t slot) const { return slots_[slot].draws.buffer; }

private:
    struct Slot {
        AllocatedBuffer rows, quads, ribbonVertices, draws, counters, read;
        uint32_t rowRoom = 0, quadRoom = 0, ribbonVertexRoom = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t blobVersion = 0, poolVersion = 0;
        bool setCurrent = false;
        /// The rows the slot's last frame ran, in order.
        std::vector<ParticleFrame::Owner> readOf;
    };
    bool ensure(Slot& slot, uint32_t rows);
    void writeSet(Slot& slot, const ParticleFrame& frame);

    VkContext* context_ = nullptr;
    const char* owner_ = "";
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline ribbonPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    Slot slots_[2];
    /// Quads and ribbon vertices a frame may lay out; grown when a frame
    /// wanted more.
    uint32_t quadRoom_ = 16384;
    uint32_t ribbonVertexRoom_ = 16384;
    std::mutex readMutex_;
    std::vector<Read> read_;
    std::atomic<uint32_t> quadsLaidOut_{0};
};

} // namespace rendering
} // namespace wowee
