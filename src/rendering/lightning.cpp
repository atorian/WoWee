#include "rendering/lightning.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"

#include <cstring>
#include <random>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering {
Lightning::Lightning() : storm_(std::random_device{}()) {}
Lightning::~Lightning() { shutdown(); }

bool Lightning::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    ctx_ = ctx;
    layout_ = createPipelineLayout(ctx_->getDevice(), {perFrameLayout}, {});
    if (!layout_) {
        LOG_ERROR("Lightning pipeline layout failed; bolts unavailable");
        return false;
    }
    for (auto& buffer : vertices_) {
        buffer = createBuffer(ctx_->getAllocator(), kVertexCapacity * sizeof(Vertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (!buffer.buffer || !buffer.info.pMappedData) {
            LOG_ERROR("Lightning vertex buffer allocation failed; bolts unavailable");
            return false;
        }
    }
    recreatePipelines();
    return pipeline_ != VK_NULL_HANDLE;
}
void Lightning::shutdown() {
    if (!ctx_) return;
    destroy(ctx_->getDevice(), pipeline_);
    destroy(ctx_->getDevice(), layout_);
    for (auto& buffer : vertices_) destroyBuffer(ctx_->getAllocator(), buffer);
    ctx_ = nullptr;
}
void Lightning::recreatePipelines() {
    if (!ctx_) return;
    destroy(ctx_->getDevice(), pipeline_);
    auto shaders = loadShaderPair(ctx_->getDevice(), "assets/shaders/lightning_bolt.vert.spv",
                                 "assets/shaders/lightning_bolt.frag.spv", "lightning_bolt");
    if (!shaders) return;
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::vector<VkVertexInputAttributeDescription> attributes{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, side)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, brightness)}};
    pipeline_ = PipelineBuilder().setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({binding}, attributes)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAdditive())
        .setMultisample(ctx_->getMsaaSamples())
        .setLayout(layout_).setRenderPass(ctx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(ctx_->getDevice(), ctx_->getPipelineCache());
    if (!pipeline_) LOG_ERROR("Lightning pipeline failed; bolts unavailable");
}
void Lightning::update(float dt, const Camera& camera, const LightningStorm::Ground& ground) {
    eye_ = camera.getPosition();
    storm_.update(dt, eye_, ground);
}
Lightning::Frame Lightning::extract() const {
    Frame frame;
    frame.ground = storm_.groundPosition();
    if (storm_.illumination() > 0.001f) {
        const glm::vec3 source = storm_.lightPosition();
        glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(145.0f), 1.0f, 2.0f, 650.0f);
        projection[1][1] *= -1.0f;
        frame.lightMatrix = projection * glm::lookAtRH(source, frame.ground, glm::vec3(0, 1, 0));
        frame.lightPosition = glm::vec4(source, 500.0f);
        frame.lightColor = glm::vec4(glm::vec3(1.1f, 1.3f, 1.7f) * storm_.illumination(), 0.0f);
    }
    if (!storm_.visible()) return frame;
    auto& mesh = frame.vertices;
    mesh.reserve(kVertexCapacity);
    for (const auto& segment : storm_.segments()) {
        const float fraction = glm::clamp((storm_.progress() - segment.begin) /
                                         std::max(segment.finish - segment.begin, 0.001f), 0.0f, 1.0f);
        if (fraction <= 0.0f) continue;
        const glm::vec3 a = segment.start;
        const glm::vec3 b = glm::mix(a, segment.end, fraction);
        glm::vec3 side = glm::cross(b - a, eye_ - (a + b) * 0.5f);
        if (glm::dot(side, side) < 1e-8f) continue;
        side = glm::normalize(side) * (segment.width * 5.0f);
        const float light = storm_.brightness();
        mesh.insert(mesh.end(), {{a - side, -1, light}, {a + side, 1, light}, {b - side, -1, light},
                                 {b - side, -1, light}, {a + side, 1, light}, {b + side, 1, light}});
    }
    return frame;
}
void Lightning::record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Frame& input) {
    if (!pipeline_ || input.vertices.empty()) return;
    const auto& mesh = input.vertices;
    if (mesh.size() > kVertexCapacity) {
        LOG_ERROR("Lightning geometry exceeded buffer capacity; strike not drawn");
        return;
    }
    // This slot is fenced before recording. Never overwrite another frame's bolt.
    auto& buffer = vertices_[ctx_->getCurrentFrame()];
    std::memcpy(buffer.info.pMappedData, mesh.data(), mesh.size() * sizeof(Vertex));
    vmaFlushAllocation(ctx_->getAllocator(), buffer.allocation, 0, mesh.size() * sizeof(Vertex));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &perFrameSet, 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.buffer, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(mesh.size()), 1, 0, 0);
}
} // namespace wowee::rendering
