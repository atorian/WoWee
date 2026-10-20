#include "rendering/interface_frame.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace wowee::rendering {
InterfaceRecorder::InterfaceRecorder() = default;
InterfaceRecorder::~InterfaceRecorder() { shutdown(); }

bool InterfaceRecorder::initialize(VkContext& ctx, VkRenderPass pass) {
    ctx_ = &ctx;
    const auto device = ctx.getDevice();
    if (!shaders_) shaders_ = std::make_unique<ShaderPair>(loadShaderPair(device,
        "assets/shaders/interface.vert.spv", "assets/shaders/interface.frag.spv", "interface"));
    if (!*shaders_) return false;
    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (!textures_ && vkCreateDescriptorSetLayout(device, &info, nullptr, &textures_) != VK_SUCCESS) return false;
    if (!layout_) layout_ = createPipelineLayout(device, {textures_}, {{VK_SHADER_STAGE_VERTEX_BIT, 0, 16}});
    if (!layout_) return false;
    VkVertexInputBindingDescription vb{0, sizeof(InterfaceFrame::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::vector<VkVertexInputAttributeDescription> attributes{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(InterfaceFrame::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(InterfaceFrame::Vertex, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(InterfaceFrame::Vertex, colour)}};
    pipeline_ = PipelineBuilder().setShaders(shaders_->vertStage, shaders_->fragStage)
        .setVertexInput({vb}, attributes).setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(false, false).setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(VK_SAMPLE_COUNT_1_BIT).setLayout(layout_).setRenderPass(pass)
        .build(device, ctx.getPipelineCache());
    pass_ = pass;
    return pipeline_ != VK_NULL_HANDLE;
}

void InterfaceRecorder::record(VkContext& ctx, VkCommandBuffer cmd, VkRenderPass pass,
                                uint32_t slot, const InterfaceFrame& frame) {
    const int width = static_cast<int>(frame.displaySize.x * frame.framebufferScale.x);
    const int height = static_cast<int>(frame.displaySize.y * frame.framebufferScale.y);
    if (width <= 0 || height <= 0 || frame.vertices.empty() || frame.indices.empty()) return;
    if (frame.resources) frame.resources->usedByFrame(ctx.frameNumber());
    if (!shaders_) throw std::logic_error("Interface recorder was not prepared before recording");
    if (pipeline_ && pass_ != pass) {
        const auto old = std::exchange(pipeline_, VK_NULL_HANDLE);
        const auto device = ctx.getDevice();
        ctx.deferAfterAllFrameFences([device, old] { vkDestroyPipeline(device, old, nullptr); });
    }
    if (!pipeline_ && !initialize(ctx, pass)) {
        LOG_ERROR("Interface recorder pipeline could not be created; interface frame is not drawn");
        throw std::runtime_error("Interface pipeline creation failed");
    }
    auto& buffers = buffers_[slot % buffers_.size()];
    auto upload = [&](AllocatedBuffer& buffer, size_t& capacity, const void* source,
                      size_t bytes, VkBufferUsageFlags usage) {
        if (bytes > capacity) {
            if (buffer.buffer) destroyBuffer(ctx.getAllocator(), buffer);
            capacity = (bytes + 65535) & ~size_t(65535);
            buffer = createBuffer(ctx.getAllocator(), capacity, usage, VMA_MEMORY_USAGE_CPU_TO_GPU);
        }
        if (!buffer.buffer || !buffer.info.pMappedData) {
            LOG_ERROR("Interface recorder buffer allocation failed for ", bytes, " bytes");
            throw std::runtime_error("Interface buffer allocation failed");
        }
        std::memcpy(buffer.info.pMappedData, source, bytes);
        vmaFlushAllocation(ctx.getAllocator(), buffer.allocation, 0, bytes);
    };
    upload(buffers.vertices, buffers.vertexBytes, frame.vertices.data(),
           frame.vertices.size() * sizeof(InterfaceFrame::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    upload(buffers.indices, buffers.indexBytes, frame.indices.data(),
           frame.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffers.vertices.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, buffers.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const glm::vec2 scale = 2.0f / frame.displaySize;
    const glm::vec4 transform(scale, -glm::vec2(1) - frame.displayPos * scale);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform), &transform);
    for (const auto& draw : frame.draws) {
        const glm::vec2 lo = glm::max((glm::vec2(draw.clip) - frame.displayPos) * frame.framebufferScale, glm::vec2(0));
        const glm::vec2 hi = glm::min((glm::vec2(draw.clip.z, draw.clip.w) - frame.displayPos) * frame.framebufferScale,
                                    glm::vec2(width, height));
        if (hi.x <= lo.x || hi.y <= lo.y || !draw.count) continue;
        VkRect2D scissor{{static_cast<int32_t>(lo.x), static_cast<int32_t>(lo.y)},
                        {static_cast<uint32_t>(hi.x - lo.x), static_cast<uint32_t>(hi.y - lo.y)}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &draw.texture, 0, nullptr);
        vkCmdDrawIndexed(cmd, draw.count, 1, draw.firstIndex, draw.vertexOffset, 0);
    }
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void InterfaceRecorder::shutdown() {
    if (!ctx_) return;
    for (auto& buffers : buffers_) {
        if (buffers.vertices.buffer) destroyBuffer(ctx_->getAllocator(), buffers.vertices);
        if (buffers.indices.buffer) destroyBuffer(ctx_->getAllocator(), buffers.indices);
        buffers = {};
    }
    destroy(ctx_->getDevice(), pipeline_);
    destroy(ctx_->getDevice(), layout_);
    destroy(ctx_->getDevice(), textures_);
    shaders_.reset();
    ctx_ = nullptr;
    pass_ = VK_NULL_HANDLE;
}
} // namespace wowee::rendering
