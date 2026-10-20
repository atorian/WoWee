#pragma once

#include "rendering/frames_in_flight.hpp"
#include "rendering/vk_utils.hpp"
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <vector>

namespace wowee::rendering {
class SnapshotLease;
class VkContext;
struct ShaderPair;

/// No pointers into ImGui, its font atlas, or the next interface frame.
struct InterfaceFrame {
    struct Vertex { glm::vec2 position, uv; uint32_t colour; };
    struct Draw {
        glm::vec4 clip;
        VkDescriptorSet texture = VK_NULL_HANDLE;
        uint32_t count = 0, firstIndex = 0;
        int32_t vertexOffset = 0;
    };
    glm::vec2 displayPos{0}, displaySize{0}, framebufferScale{1};
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Draw> draws;
    std::vector<std::shared_ptr<const void>> textures;
    std::shared_ptr<SnapshotLease> resources;
};

/// Only GPU state belongs here. A recorder needs no ImGui context/backend.
class InterfaceRecorder {
public:
    InterfaceRecorder();
    ~InterfaceRecorder();
    bool initialize(VkContext& ctx, VkRenderPass pass);
    void record(VkContext& ctx, VkCommandBuffer cmd, VkRenderPass pass,
                uint32_t slot, const InterfaceFrame& frame);
    void shutdown();
private:
    struct Buffers { AllocatedBuffer vertices, indices; size_t vertexBytes = 0, indexBytes = 0; };
    std::array<Buffers, MAX_FRAMES_IN_FLIGHT> buffers_;
    VkContext* ctx_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    std::unique_ptr<ShaderPair> shaders_;
};
} // namespace wowee::rendering
