#pragma once
#include "rendering/vk_utils.hpp"
#include "rendering/frames_in_flight.hpp"
#include "rendering/rt_lighting.hpp"
#include <memory>
#include <glm/glm.hpp>
#include <array>
#include <vector>
#include "rendering/surface_identity.hpp"

namespace wowee::rendering {
class VkContext;
class SurfaceRt;
// Transient evaluated surfaces, shared on the graphics queue. Each frame slot
// owns its material upload. Family workers write disjoint material ranges and
// command pools; shared images/RT are executed in authored order on one queue.
class SurfacePass {
public:
    enum class Family : size_t { Wmo, M2, Count };
    static constexpr size_t kFamilies=size_t(Family::Count);
    struct Material {
        std::array<uint32_t,16> words;
        glm::uvec4 localLights;
        glm::mat4 previousFromCurrent{1};
        glm::uvec4 identity{0}, history{0};
    };
    struct M2Material {
        glm::ivec4 flags{0}; // unlit, foliage; reserved
        glm::vec4 factors{0}; // specular, emissive, highlight; reserved
        glm::vec4 interiorAmbient{0}, interiorDirect{0};
        glm::uvec4 localLights{0};
        glm::mat4 previousFromCurrent{1};
        glm::uvec4 identity{0}, history{0};
    };
    struct TargetVersion {
        uint64_t swapchain, postProcess;
        bool offscreen;
        bool operator==(const TargetVersion&) const = default;
    };
    SurfacePass(VkContext& ctx, VkDescriptorSetLayout frameLayout);
    ~SurfacePass();
    void prepare(VkExtent2D extent, VkImageView depth, TargetVersion version, std::array<size_t,kFamilies> materialCapacities,
                 RtScene* rtScene, int rtMode, const RtLighting::FrameInputs& rtInputs);
    bool rtHistoryValid() const;
    glm::vec3 previousRtCamera() const;
    uint32_t appendMaterial(const Material& material);
    uint32_t appendMaterial(const M2Material& material);
    void finishMaterials();
    VkRenderPass geometryPass() const { return pass_; }
    // prepare runs on the render owner after the slot fence; each family worker
    // records its runs. finishMaterials runs after the workers join; execute
    // only walks commands, never the producer scene.
    VkCommandBuffer beginRun(Family family, bool geometry);
    void endRun(Family family);
    void execute(Family family, VkCommandBuffer cmd, VkDescriptorSet frameSet, VkFramebuffer scene,
                 const glm::mat4& inverseViewProjection, VkSubpassContents resumeContents);
private:
    void beginGeometry(VkCommandBuffer cmd);
    uint32_t appendMaterialBytes(Family family, const void* material);
    void light(Family family, VkCommandBuffer cmd, VkDescriptorSet frameSet, VkFramebuffer scene,
               const glm::mat4& inverseViewProjection);
    void resumeScene(VkCommandBuffer cmd, VkFramebuffer scene, VkSubpassContents contents);
    void retire();
    void create(VkExtent2D extent, VkImageView depth);
    VkContext& ctx_;
    VkDescriptorSetLayout frameLayout_;
    VkExtent2D extent_{};
    VkImageView depth_ = VK_NULL_HANDLE;
    TargetVersion targetVersion_{};
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED, colorFormat_ = VK_FORMAT_UNDEFINED;
    std::array<AllocatedImage,3> images_{};
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline,kFamilies> pipelines_{};
    static constexpr size_t kSlots = MAX_FRAMES_IN_FLIGHT;
    std::array<AllocatedBuffer,kSlots> buffers_{};
    std::array<size_t,kSlots> capacities_{};
    std::array<VkDescriptorSet,kSlots> sets_{};
    struct Run { VkCommandBuffer command; bool geometry; };
    struct Commands {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
        std::vector<Run> runs;
    };
    std::array<std::array<Commands,kFamilies>,kSlots> commands_{};
    std::unique_ptr<SurfaceRt> rt_;
    int rtMode_ = 0;
    float rtDistance_ = 0;
    std::array<size_t,kFamilies> materialBegin_{}, materialLimit_{}, materialsWritten_{};
};
}
