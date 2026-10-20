#pragma once

#include "rendering/per_frame_buffer.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>

namespace wowee {
namespace rendering {

class Camera;
class VkContext;

class MountDust {
public:
    MountDust();
    ~MountDust();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    /// The pipeline state both initialize() and
    /// recreatePipelines() need, described once.
    void buildPipelines(VkDevice device,
                        const VkPipelineShaderStageCreateInfo& vertStage,
                        const VkPipelineShaderStageCreateInfo& fragStage);

    // Spawn dust particles at mount feet when moving on ground
    void spawnDust(const glm::vec3& position, const glm::vec3& velocity, bool isMoving);

    void update(float deltaTime);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

private:
    struct Particle {
        glm::vec3 position;
        glm::vec3 velocity;
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
    };

    static constexpr int MAX_DUST_PARTICLES = 300;
    std::vector<Particle> particles;

    // Vulkan objects
    VkContext* vkCtx = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    // Particle vertex data, rewritten every frame
    PerFrameBuffer dynamicVB;

    std::vector<float> vertexData;
    float spawnAccum = 0.0f;
};

} // namespace rendering
} // namespace wowee
