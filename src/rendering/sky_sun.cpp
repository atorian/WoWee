#include "rendering/sky_sun.hpp"

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

constexpr const char* kDiscTexture = "Textures\\sunCenter.blp";
constexpr const char* kGlareTexture = "Textures\\sunGlare.blp";

}  // namespace

SkySun::~SkySun() { shutdown(); }

VkPipeline SkySun::buildPipeline(bool glare) {
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
        // says whether the sun can be seen.
        builder.setDepthTest(false, false, VK_COMPARE_OP_ALWAYS)
            .setColorBlendAttachment(PipelineBuilder::blendAdditive());
    } else {
        // With the sky: on the far plane, behind whatever the world drew.
        builder.setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(PipelineBuilder::blendAlpha());
    }
    return builder.build(device, vkCtx_->getPipelineCache());
}

bool SkySun::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx_ = ctx;
    VkDevice device = vkCtx_->getDevice();

    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureSetLayout_ = createDescriptorSetLayout(device, {sampler});

    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .descriptorCount = 2};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (!textureSetLayout_ ||
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        LOG_ERROR("SkySun: could not create its texture descriptors - no sun disc or glare");
        return false;
    }
    VkDescriptorSetLayout layouts[2] = {textureSetLayout_, textureSetLayout_};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts = layouts;
    VkDescriptorSet sets[2] = {};
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        LOG_ERROR("SkySun: could not allocate its texture sets - no sun disc or glare");
        return false;
    }
    discSetAllocated_ = sets[0];
    glareSetAllocated_ = sets[1];

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(SpritePush);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout, textureSetLayout_}, {push});
    if (!pipelineLayout_) {
        LOG_ERROR("SkySun: could not create its pipeline layout - no sun disc or glare");
        return false;
    }
    discPipeline_ = buildPipeline(false);
    glarePipeline_ = buildPipeline(true);
    if (!discPipeline_ || !glarePipeline_) {
        LOG_ERROR("SkySun: could not build its pipelines - no sun disc or glare");
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

bool SkySun::loadTexture(pipeline::AssetManager* assets, const char* path, VkTexture& texture,
                         VkDescriptorSet set, VkDescriptorSet& published) {
    published = VK_NULL_HANDLE;
    if (!set) return false;
    const pipeline::BLPImage blp = assets->loadTexture(path);
    if (!blp.isValid()) {
        LOG_WARNING("SkySun: ", path, " did not load - that part of the sun is not drawn");
        return false;
    }
    VkDevice device = vkCtx_->getDevice();
    if (!texture.upload(*vkCtx_, blp.data.data(), blp.width, blp.height,
                        VK_FORMAT_R8G8B8A8_UNORM, true)) {
        LOG_WARNING("SkySun: ", path, " could not be uploaded - that part of the sun is not drawn");
        return false;
    }
    texture.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                          VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (!texture.isValid()) {
        LOG_WARNING("SkySun: ", path, " is not sampleable - that part of the sun is not drawn");
        return false;
    }
    VkDescriptorImageInfo image = texture.descriptorInfo();
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    published = set;
    return true;
}

void SkySun::loadTextures(pipeline::AssetManager* assets) {
    if (!vkCtx_ || !assets) return;
    if (!discSet_) loadTexture(assets, kDiscTexture, discTexture_, discSetAllocated_, discSet_);
    if (!glareSet_) loadTexture(assets, kGlareTexture, glareTexture_, glareSetAllocated_, glareSet_);
}

void SkySun::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, discPipeline_);
    destroy(device, glarePipeline_);
    discPipeline_ = buildPipeline(false);
    glarePipeline_ = buildPipeline(true);
    if (!discPipeline_ || !glarePipeline_) {
        LOG_ERROR("SkySun: could not rebuild its pipelines - no sun disc or glare");
    }
}

void SkySun::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkDeviceWaitIdle(device);
    discTexture_.destroy(device, allocator);
    glareTexture_.destroy(device, allocator);
    discSet_ = glareSet_ = discSetAllocated_ = glareSetAllocated_ = VK_NULL_HANDLE;
    destroy(allocator, vertexBuffer_, vertexAlloc_);
    destroy(allocator, indexBuffer_, indexAlloc_);
    destroy(device, discPipeline_);
    destroy(device, glarePipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, descriptorPool_);
    destroy(device, textureSetLayout_);
    vkCtx_ = nullptr;
}

void SkySun::bindAndDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet perFrameSet,
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

void SkySun::renderDisc(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                        const sky_celestial::SunDisc& disc, const glm::vec3& colour,
                        float alpha) {
    lastDiscDraw_ = {.drawn = false, .skipped = "", .direction = disc.direction, .alpha = alpha};
    if (!discPipeline_) { lastDiscDraw_.skipped = "no pipeline"; return; }
    if (!discSet_) { lastDiscDraw_.skipped = "no texture"; return; }
    if (!sky_celestial::discAboveHorizon(disc)) { lastDiscDraw_.skipped = "below the horizon"; return; }
    if (alpha <= 0.0f) { lastDiscDraw_.skipped = "put out by weather"; return; }

    SpritePush push{};
    push.model = skyBillboard(disc.direction * kSpriteDistance, disc.scale * kUnitsPerClientUnit);
    push.colour = glm::vec4(colour, alpha);
    push.horizon = glm::vec4(disc.horizonHeight, disc.scale, 1.0f, 0.0f);
    bindAndDraw(cmd, discPipeline_, perFrameSet, discSet_, push);
    lastDiscDraw_.drawn = true;
}

void SkySun::renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                         const glm::vec3& direction, float size, const glm::vec3& colour,
                         float alpha) {
    if (!glarePipeline_ || !glareSet_ || alpha <= 0.0f) return;
    SpritePush push{};
    push.model = skyBillboard(direction * kSpriteDistance, size * kUnitsPerClientUnit);
    push.colour = glm::vec4(colour, alpha);
    push.horizon = glm::vec4(0.0f);
    bindAndDraw(cmd, glarePipeline_, perFrameSet, glareSet_, push);
}

}  // namespace rendering
}  // namespace wowee
