#pragma once

// Poses a frame's skeletons on the GPU (skeleton.comp.glsl) - the only place
// skeletons are made - for a renderer that keeps its models' bones in a
// SkeletonPool: the renderer lists what to pose at extraction (a
// SkeletonFrame), and the pass, on the render thread after the frame slot's
// fence, writes the requests, dispatches a level at a time and copies back
// the bones the CPU reads. The draws bind drawSet() and read bones and each
// request's model matrix from it.
//
// What the CPU reads of a skeleton comes back two frames late: the bones a
// slot's frame copied back are taken when the slot comes round again.

#include "rendering/vk_utils.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

/// One skeleton to pose, as skeleton.comp reads it (Request there, std430).
struct SkeletonRequestGPU {
    /// Bit 0: turn billboard bones to the view. Bit 1: the arms on their own
    /// sequences. Bit 2: no skeleton - placed, not posed. Bit 3: a scale key
    /// under 0.001 is 1 (the doodads' rule).
    static constexpr uint32_t kTurnBillboards = 1u;
    static constexpr uint32_t kArmsOwnSequences = 2u;
    static constexpr uint32_t kNoSkeleton = 4u;
    static constexpr uint32_t kTinyScaleIsOne = 8u;

    uint32_t page = 0, blob = 0, outBase = 0, flags = 0;
    int32_t sequence = -1;
    float time = 0.0f, globalTime = 0.0f, torsoYaw = 0.0f;
    glm::ivec2 armSequence{-1};
    glm::vec2 armTime{0.0f};
    int32_t parent = -1;
    uint32_t parentBone = ~0u;
    uint32_t pad0 = 0, pad1 = 0;
    glm::mat4 placement{1.0f};
    /// Moved by this in the world after it is placed: a weapon on the back
    /// eased out of the arm swinging into it.
    glm::vec4 shift{0.0f};
};

/// What one frame poses: its requests, those placed by their own matrix
/// first and then each level that rides the one before (levelEnds[i] is
/// where level i ends), boneCount bones in all, the pool's pages as they
/// were, and the bones copied back for the CPU.
struct SkeletonFrame {
    struct Read {
        uint32_t owner;     ///< whose, as the renderer names it
        uint16_t bone;
        uint32_t source;    ///< where in the frame's bones
    };
    std::vector<SkeletonRequestGPU> requests;
    std::vector<uint32_t> levelEnds;
    uint32_t boneCount = 0;
    glm::mat4 view{1.0f};
    std::vector<::VkBuffer> pages;
    uint32_t pagesVersion = 0;
    std::vector<Read> reads;
    void clear() noexcept;
};

class SkeletonPass {
public:
    /// A bone read back: whose, which, and its matrix in model space.
    struct ReadBone {
        uint32_t owner;
        uint16_t bone;
        glm::mat4 matrix;
    };

    SkeletonPass() = default;
    SkeletonPass(const SkeletonPass&) = delete;
    SkeletonPass& operator=(const SkeletonPass&) = delete;

    /// The pipeline and each frame slot's sets. False, said, when one could
    /// not be made: nothing posed is drawn.
    bool initialize(VkContext& context, const char* owner);
    /// Everything let go after the frames in flight.
    void destroy();

    /// The draws' set: binding 0 the frame's bones, binding 1 each request's
    /// model matrix. Vertex stage.
    [[nodiscard]] VkDescriptorSetLayout drawSetLayout() const { return drawSetLayout_; }
    [[nodiscard]] VkDescriptorSet drawSet(uint32_t slot) const { return slots_[slot].drawSet; }

    /// Takes what the slot's last frame copied back, and poses `frame` into
    /// the slot's buffers. Outside a render pass; false, said, when there
    /// was no memory - nothing of the frame is posed then.
    bool record(VkCommandBuffer cmd, uint32_t slot, const SkeletonFrame& frame);
    /// The bones read back since the last call.
    [[nodiscard]] std::vector<ReadBone> takeReadBones();

private:
    struct Slot {
        AllocatedBuffer requests, bones, models, readback;
        uint32_t requestRoom = 0, modelRoom = 0, boneRoom = 0, readRoom = 0;
        VkDescriptorSet computeSet = VK_NULL_HANDLE;
        VkDescriptorSet drawSet = VK_NULL_HANDLE;
        uint32_t pagesVersion = 0;
        bool setsCurrent = false;
        /// The bones the slot's last frame copied back, in readback order.
        std::vector<std::pair<uint32_t, uint16_t>> readOf;
    };
    bool ensure(Slot& slot, uint32_t requests, uint32_t bones, uint32_t reads);

    VkContext* context_ = nullptr;
    const char* owner_ = "";
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout computeSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout drawSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    Slot slots_[2];
    std::mutex readMutex_;
    std::vector<ReadBone> read_;
};

} // namespace rendering
} // namespace wowee
