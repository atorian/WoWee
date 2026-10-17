#include "rendering/sky_bodies.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/sky_billboard.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

#include <vector>

namespace wowee {
namespace rendering {

namespace {

/// How far out the sky's sprites are drawn here. The view's translation is
/// removed, so any distance inside the far plane does; the client's twelve
/// units are scaled up to it.
constexpr float kSpriteDistance = 800.0f;
constexpr float kUnitsPerClientUnit = kSpriteDistance / sky_celestial::kSkyDistance;

}  // namespace

SkyBodies::~SkyBodies() { shutdown(); }

VkPipeline SkyBodies::buildPipeline(bool glare) {
    VkDevice device = vkCtx_->getDevice();
    auto shaders = loadShaderPair(device, "assets/shaders/sky_sprite.vert.spv",
                                  "assets/shaders/sky_sprite.frag.spv", "sky sprite");
    if (!shaders) return VK_NULL_HANDLE;

    PipelineBuilder builder;
    builder.setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({tightVertexBinding(5 * sizeof(float))}, positionPlusUvAttrs())
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic());
    if (glare) {
        // Over the world, added, and not depth tested: the caller's visibility
        // says whether the body can be seen.
        builder.setDepthTest(false, false, VK_COMPARE_OP_ALWAYS)
            .setColorBlendAttachment(PipelineBuilder::blendAdditive());
    } else {
        // With the sky: on the far plane, behind whatever the world drew.
        builder.setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(PipelineBuilder::blendAlpha());
    }
    return builder.build(device, vkCtx_->getPipelineCache());
}

bool SkyBodies::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx_ = ctx;
    VkDevice device = vkCtx_->getDevice();

    discSprite(Body::Sun).path = "Textures\\sunCenter.blp";
    glareSprite(Body::Sun).path = "Textures\\sunGlare.blp";
    discSprite(Body::Moon).path = "Textures\\moon.blp";
    glareSprite(Body::Moon).path = "Textures\\moonGlare.blp";

    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureSetLayout_ = createDescriptorSetLayout(device, {sampler});

    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .descriptorCount = kSpriteCount};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kSpriteCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (!textureSetLayout_ ||
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        LOG_ERROR("SkyBodies: could not create its texture descriptors - no sun or moon drawn");
        return false;
    }
    VkDescriptorSetLayout layouts[kSpriteCount];
    for (auto& layout : layouts) layout = textureSetLayout_;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = kSpriteCount;
    allocInfo.pSetLayouts = layouts;
    VkDescriptorSet sets[kSpriteCount] = {};
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        LOG_ERROR("SkyBodies: could not allocate its texture sets - no sun or moon drawn");
        return false;
    }
    for (int i = 0; i < kSpriteCount; ++i) sprites_[i].allocated = sets[i];

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(SpritePush);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout, textureSetLayout_}, {push});
    if (!pipelineLayout_) {
        LOG_ERROR("SkyBodies: could not create its pipeline layout - no sun or moon drawn");
        return false;
    }
    discPipeline_ = buildPipeline(false);
    glarePipeline_ = buildPipeline(true);
    if (!discPipeline_ || !glarePipeline_) {
        LOG_ERROR("SkyBodies: could not build its pipelines - no sun or moon drawn");
        return false;
    }

    // The client's unit quad: v = 0 along the top edge (0x7edbe0).
    const float vertices[] = {
        -0.5f,  0.5f, 0.0f,   0.0f, 0.0f,
         0.5f,  0.5f, 0.0f,   1.0f, 0.0f,
         0.5f, -0.5f, 0.0f,   1.0f, 1.0f,
        -0.5f, -0.5f, 0.0f,   0.0f, 1.0f,
    };
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    AllocatedBuffer vbuf = uploadBuffer(*vkCtx_, vertices, sizeof(vertices),
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    AllocatedBuffer ibuf = uploadBuffer(*vkCtx_, indices, sizeof(indices),
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    vertexBuffer_ = vbuf.buffer;
    vertexAlloc_ = vbuf.allocation;
    indexBuffer_ = ibuf.buffer;
    indexAlloc_ = ibuf.allocation;
    return vertexBuffer_ && indexBuffer_;
}

void SkyBodies::loadSprite(pipeline::AssetManager* assets, Sprite& sprite) {
    if (sprite.set || !sprite.allocated) return;
    const pipeline::BLPImage blp = assets->loadTexture(sprite.path);
    if (!blp.isValid()) {
        LOG_WARNING("SkyBodies: ", sprite.path, " did not load - that sprite is not drawn");
        return;
    }
    VkDevice device = vkCtx_->getDevice();
    if (!sprite.texture.upload(*vkCtx_, blp.data.data(), blp.width, blp.height,
                               VK_FORMAT_R8G8B8A8_UNORM, true)) {
        LOG_WARNING("SkyBodies: ", sprite.path, " could not be uploaded - that sprite is not drawn");
        return;
    }
    sprite.texture.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (!sprite.texture.isValid()) {
        LOG_WARNING("SkyBodies: ", sprite.path, " is not sampleable - that sprite is not drawn");
        return;
    }
    VkDescriptorImageInfo image = sprite.texture.descriptorInfo();
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sprite.allocated;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    sprite.set = sprite.allocated;
}

void SkyBodies::loadTextures(pipeline::AssetManager* assets) {
    if (!vkCtx_ || !assets) return;
    for (Sprite& sprite : sprites_) loadSprite(assets, sprite);
}

void SkyBodies::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, discPipeline_);
    destroy(device, glarePipeline_);
    discPipeline_ = buildPipeline(false);
    glarePipeline_ = buildPipeline(true);
    if (!discPipeline_ || !glarePipeline_) {
        LOG_ERROR("SkyBodies: could not rebuild its pipelines - no sun or moon drawn");
    }
}

void SkyBodies::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkDeviceWaitIdle(device);
    for (Sprite& sprite : sprites_) {
        sprite.texture.destroy(device, allocator);
        sprite.set = sprite.allocated = VK_NULL_HANDLE;
    }
    destroy(allocator, vertexBuffer_, vertexAlloc_);
    destroy(allocator, indexBuffer_, indexAlloc_);
    destroy(device, discPipeline_);
    destroy(device, glarePipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, descriptorPool_);
    destroy(device, textureSetLayout_);
    vkCtx_ = nullptr;
}

void SkyBodies::bindAndDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet perFrameSet,
                            VkDescriptorSet textureSet, const SpritePush& push) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet sets[2] = {perFrameSet, textureSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets,
                            0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
}

void SkyBodies::renderDisc(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, Body body,
                           const sky_celestial::SkyDisc& disc, const glm::vec3& colour,
                           float alpha) {
    DiscDraw& report = lastDiscDraw_[static_cast<int>(body)];
    report = {.drawn = false, .skipped = "", .direction = disc.direction, .alpha = alpha};
    const Sprite& sprite = discSprite(body);
    if (!discPipeline_) { report.skipped = "no pipeline"; return; }
    if (!sprite.set) { report.skipped = "no texture"; return; }
    if (!sky_celestial::discAboveHorizon(disc)) { report.skipped = "below the horizon"; return; }
    if (alpha <= 0.0f) { report.skipped = "put out by weather"; return; }

    SpritePush push{};
    push.model = skyBillboard(disc.direction * kSpriteDistance, disc.scale * kUnitsPerClientUnit);
    push.colour = glm::vec4(colour, alpha);
    push.horizon = glm::vec4(disc.horizonHeight, disc.scale, 1.0f, 0.0f);
    bindAndDraw(cmd, discPipeline_, perFrameSet, sprite.set, push);
    report.drawn = true;
}

void SkyBodies::renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, Body body,
                            const glm::vec3& direction, float size, const glm::vec3& colour,
                            float alpha) {
    const Sprite& sprite = glareSprite(body);
    if (!glarePipeline_ || !sprite.set || alpha <= 0.0f) return;
    SpritePush push{};
    push.model = skyBillboard(direction * kSpriteDistance, size * kUnitsPerClientUnit);
    push.colour = glm::vec4(colour, alpha);
    push.horizon = glm::vec4(0.0f);
    bindAndDraw(cmd, glarePipeline_, perFrameSet, sprite.set, push);
}

}  // namespace rendering
}  // namespace wowee
