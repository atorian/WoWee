#include "rendering/precipitation.hpp"
#include "rendering/liquid_scene.hpp"
#include "rendering/shelter_map.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"

#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

/// Must match Push in precipitation.vert.glsl.
struct PrecipitationPush {
    glm::vec4 cameraBox;
    glm::vec4 driftHeight;
    glm::vec4 velocity;
    glm::vec4 shape;
    glm::vec4 shelter;
    glm::vec4 shelterRange;
    glm::vec4 tint;
    glm::vec4 screen;
};
static_assert(sizeof(PrecipitationPush) == 128, "the push constant block is 128 bytes at most");

/// What black rain and snow are tinted.
constexpr glm::vec3 kDarkTint{0.28f, 0.25f, 0.22f};
/// Sand blown at less than this is still driven at it: a sandstorm is wind.
constexpr float kLeastSandSpeed = 9.0f;
/// The shaders' time wraps here: a whole number of splash lives (0.4 s).
constexpr double kTimeWrap = 1000.0;
/// A cell of the water grid with no water over it (precipitation.vert.glsl).
constexpr float kNoWater = -1e9f;
/// How long the open sky takes to come over to a new place, in seconds.
constexpr float kOpenSkySeconds = 1.0f;
/// What is left of the wind with the sky closed in all round.
constexpr float kWalledInWind = 0.35f;

double wrapPositive(double v, double period) {
    const double r = std::fmod(v, period);
    return r < 0.0 ? r + period : r;
}

}  // namespace

Precipitation::~Precipitation() {
    shutdown();
}

bool Precipitation::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, const ShelterMap& shelter) {
    ctx_ = ctx;
    shelter_ = &shelter;
    VkDevice device = ctx_->getDevice();
    if (targetPass_ == VK_NULL_HANDLE) {
        targetPass_ = ctx_->getImGuiRenderPass();
        targetSamples_ = ctx_->getMsaaSamples();
    }

    using K = LayerKind;
    // kind, drops at full, box width, box height, below, size, fall, wind, streak, opacity, seed, left
    // when walled in
    layers_[0] = {K::Rain, 7000, 22.0f, 18.0f, 0.35f, 0.006f, 10.0f, 0.9f, 0.045f, 0.5f, 1, 0.55f};
    layers_[1] = {K::Rain, 9000, 64.0f, 40.0f, 0.3f, 0.011f, 10.0f, 0.9f, 0.06f, 0.35f, 2, 0.2f};
    layers_[2] = {K::Snow, 5000, 20.0f, 14.0f, 0.35f, 0.04f, 1.2f, 1.0f, 0.0f, 0.9f, 3, 0.55f};
    layers_[3] = {K::Snow, 9000, 60.0f, 34.0f, 0.3f, 0.06f, 1.2f, 1.0f, 0.0f, 0.8f, 4, 0.2f};
    layers_[4] = {K::Grains, 9000, 26.0f, 8.0f, 0.5f, 0.005f, 0.3f, 1.4f, 0.015f, 0.4f, 5, 0.35f};
    layers_[5] = {K::Sheets, 70, 90.0f, 6.0f, 0.5f, 9.0f, 0.0f, 1.0f, 0.0f, 0.2f, 6, 0.25f};
    layers_[6] = {K::Splash, 450, 16.0f, 0.0f, 0.0f, 0.18f, 0.0f, 0.0f, 0.0f, 0.55f, 7, 0.6f};

    const VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
    };
    setLayout_ = createDescriptorSetLayout(device, {bindings[0], bindings[1]});
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0,
                                   .size = sizeof(PrecipitationPush)};
    layout_ = createPipelineLayout(device, {perFrameLayout, setLayout_}, {push});
    if (setLayout_ == VK_NULL_HANDLE || layout_ == VK_NULL_HANDLE) {
        LOG_ERROR("Precipitation: could not create its layouts - no rain, snow or sand drawn");
        return false;
    }

    const VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES_IN_FLIGHT},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR("Precipitation: could not create its descriptor pool - no rain, snow or sand drawn");
        return false;
    }
    const VkDescriptorImageInfo shelterInfo{.sampler = shelter.sampler(), .imageView = shelter.view(),
                                            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &setLayout_;
        waterBuffers_[slot] = createBuffer(ctx_->getAllocator(), sizeof(WaterGrid), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (vkAllocateDescriptorSets(device, &alloc, &sets_[slot]) != VK_SUCCESS ||
            !waterBuffers_[slot].info.pMappedData) {
            LOG_ERROR("Precipitation: could not allocate its sets and buffers - no rain, snow or sand drawn");
            return false;
        }
        const VkDescriptorBufferInfo waterInfo{.buffer = waterBuffers_[slot].buffer, .offset = 0,
                                               .range = VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]{};
        for (auto& w : writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = sets_[slot];
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &shelterInfo;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &waterInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    return buildPipeline();
}

bool Precipitation::buildPipeline() {
    VkDevice device = ctx_->getDevice();
    auto shaders = loadShaderPair(device, "assets/shaders/precipitation.vert.spv",
                                  "assets/shaders/precipitation.frag.spv", "precipitation");
    if (!shaders) return false;
    pipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(layout_)
        .setRenderPass(targetPass_)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, ctx_->getPipelineCache());
    if (pipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("Precipitation: could not build its pipeline - no rain, snow or sand drawn");
        return false;
    }
    return true;
}

void Precipitation::recreatePipelines() {
    if (!ctx_) return;
    DeferredResources retired(*ctx_);
    retired.destroy(pipeline_);
    (void)buildPipeline();
}

void Precipitation::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    ctx_->waitIdle();
    destroy(device, pipeline_);
    destroy(device, layout_);
    destroy(device, pool_);
    sets_ = {};
    for (AllocatedBuffer& b : waterBuffers_) destroyBuffer(ctx_->getAllocator(), b);
    destroy(device, setLayout_);
    ctx_ = nullptr;
}

void Precipitation::update(const PrecipitationFrame& frame) {
    frame_ = frame;
    const float dt = std::max(frame.dt, 0.0f);
    time_ = wrapPositive(time_ + dt, kTimeWrap);
    tint_ = glm::mix(glm::vec3(1.0f), kDarkTint, std::clamp(frame.mix.dark, 0.0f, 1.0f));
    // Over about a second: walking out of an alley opens the rain up as the
    // sky opens up, not at a step.
    const float openSky = std::clamp(frame.openSky, 0.0f, 1.0f);
    openSky_ = openSkyKnown_ ? openSky_ + (openSky - openSky_) * std::min(dt / kOpenSkySeconds, 1.0f) : openSky;
    openSkyKnown_ = true;
    // Walls break the wind as well as hide the rain.
    const glm::vec2 wind = frame.wind * glm::mix(kWalledInWind, 1.0f, openSky_);

    drawnCount_ = 0;
    for (Layer& layer : layers_) {
        float amount = 0.0f;
        switch (layer.kind) {
            case LayerKind::Rain:
            case LayerKind::Splash: amount = frame.mix.rain; break;
            case LayerKind::Snow: amount = frame.mix.snow; break;
            case LayerKind::Grains:
            case LayerKind::Sheets: amount = frame.mix.sand; break;
        }
        amount = std::clamp(amount, 0.0f, 1.0f);
        if (layer.kind == LayerKind::Splash && !splashes_) amount = 0.0f;
        layer.count = static_cast<uint32_t>(static_cast<float>(layer.fullCount) * amount * densityScale_ *
                                            glm::mix(layer.enclosed, 1.0f, openSky_));
        layer.alpha = layer.opacity * (0.6f + 0.4f * amount);

        if (layer.kind == LayerKind::Grains || layer.kind == LayerKind::Sheets) {
            const float speed = glm::length(wind);
            const glm::vec2 toward = speed > 0.1f ? wind / speed : glm::vec2(1.0f, 0.0f);
            const float driven = layer.kind == LayerKind::Grains ? std::max(speed * layer.windFollow, kLeastSandSpeed)
                                                                 : std::max(speed, kLeastSandSpeed * 0.5f);
            layer.velocity = glm::vec3(toward * driven, -layer.fallSpeed);
        } else {
            layer.velocity = glm::vec3(wind * layer.windFollow, -layer.fallSpeed);
        }
        // Wrapped at ten boxes, so each of the drops' speeds - tenths of the
        // layer's - wraps a whole number of boxes and none of them jumps.
        if (layer.boxWidth > 0.0f && layer.boxHeight > 0.0f) {
            layer.drift += glm::dvec3(layer.velocity) * static_cast<double>(dt);
            layer.drift.x = wrapPositive(layer.drift.x, 10.0 * layer.boxWidth);
            layer.drift.y = wrapPositive(layer.drift.y, 10.0 * layer.boxWidth);
            layer.drift.z = wrapPositive(layer.drift.z, 10.0 * layer.boxHeight);
        }
        drawnCount_ += layer.count;
    }
    if (frame.mix.rain > 0.0f && splashes_) lookAtWater(frame);
}

void Precipitation::lookAtWater(const PrecipitationFrame& frame) {
    // The water does not move; the camera does. Looked at again as the
    // camera crosses a cell, and every second for the tiles streamed in.
    const float cell = 2.0f * kWaterReach / static_cast<float>(kWaterCells);
    const glm::vec2 here(frame.camera);
    if (glm::length(here - waterCentre_) < cell && frame.now - waterLookedAt_ < 1.0) return;
    waterCentre_ = here;
    waterLookedAt_ = frame.now;
    const glm::vec2 low = here - glm::vec2(kWaterReach);
    water_.origin = glm::vec4(low, cell, static_cast<float>(kWaterCells));
    for (int y = 0; y < kWaterCells; ++y) {
        for (int x = 0; x < kWaterCells; ++x) {
            const glm::vec2 at = low + (glm::vec2(x, y) + 0.5f) * cell;
            const std::optional<float> h = frame.water ? frame.water->heightAt(at.x, at.y) : std::nullopt;
            water_.heights[y * kWaterCells + x] = h.value_or(kNoWater);
        }
    }
}

Precipitation::Frame Precipitation::extract() const {
    return {layers_, water_, frame_.camera, tint_, frame_.viewport, time_, drawnCount_};
}

void Precipitation::record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, uint32_t frameSlot, const Frame& input) {
    ZoneScopedN("Precipitation::render");
    if (pipeline_ == VK_NULL_HANDLE || input.drawn == 0 || !shelter_ || frameSlot >= sets_.size()) return;
    std::memcpy(waterBuffers_[frameSlot].info.pMappedData, &input.water, sizeof(input.water));
    if (!firstDrawSaid_) {
        firstDrawSaid_ = true;
        LOG_INFO("Precipitation: first drawn, ", input.drawn, " drops in a ", input.viewport.width, "x",
                 input.viewport.height, " view");
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDescriptorSet sets[2] = {perFrameSet, sets_[frameSlot]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);

    const OverheadFrame& map = shelter_->frame();
    PrecipitationPush push{};
    push.shelter = glm::vec4(map.centre, map.halfExtent, map.ceiling);
    push.shelterRange = glm::vec4(map.range, shelter_->isDrawn() ? 1.0f : 0.0f, 0.0f, 0.0f);
    push.screen = glm::vec4(static_cast<float>(input.viewport.width), static_cast<float>(input.viewport.height),
                            0.0f, 0.0f);
    for (const Layer& layer : input.layers) {
        if (layer.count == 0) continue;
        push.cameraBox = glm::vec4(input.camera, layer.boxWidth);
        push.driftHeight = glm::vec4(glm::vec3(layer.drift), layer.boxHeight);
        push.velocity = glm::vec4(layer.velocity, layer.streakSeconds);
        push.shape = glm::vec4(static_cast<float>(layer.kind), layer.size, layer.alpha, static_cast<float>(input.time));
        push.tint = glm::vec4(input.tint, static_cast<float>(layer.seed));
        push.screen.z = layer.below;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, layer.count * 6u, 1, 0, 0);
    }
}

}  // namespace rendering
}  // namespace wowee
