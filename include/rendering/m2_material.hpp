#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace wowee::rendering {

// Skin batches address a combo table, not the transparency array directly.
// Zero is an authored invisible layer and must not become the default one.
inline float m2TextureWeightOpacity(const std::vector<float>& weights,
                                   const std::vector<uint16_t>& lookup,
                                   uint16_t combo) {
    if (combo >= lookup.size() || lookup[combo] >= weights.size()) return 1.0f;
    const float value = weights[lookup[combo]];
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
}

// Preserve source order within an authored priority plane so each base,
// reflection and decal stack keeps its layer order. Keep the batch array itself
// unchanged: sky diagnostics and material descriptors refer to its indices.
template<class Batch>
inline std::vector<size_t> m2MaterialDrawOrder(const std::vector<Batch>& batches) {
    std::vector<size_t> order(batches.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return batches[a].priorityPlane < batches[b].priorityPlane;
    });
    return order;
}

// Only opaque/cutout geometry casts a directional shadow. Blended reflection
// overlays and luminous cones do not become opaque blockers in the depth pass.
inline bool m2BatchCastsShadow(uint16_t blendMode, float opacity,
                               bool glowCard, bool starLayer) {
    return blendMode <= 1 && opacity >= 0.01f && !glowCard && !starLayer;
}

// Modulation ignores texture alpha and preserves destination alpha.
inline VkPipelineColorBlendAttachmentState m2ModulateBlendState(bool twice) {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    state.dstColorBlendFactor = twice ? VK_BLEND_FACTOR_SRC_COLOR : VK_BLEND_FACTOR_ZERO;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    return state;
}

// M2Texture flags: bit 0 repeats U, bit 1 repeats V; a clear bit clamps.
//
// The wrap belongs to the texture slot of one model, not to the image: the
// same BLP is clamped by one model and tiled by another. So a batch samples
// through a sampler built from its own flags, not through whatever sampler the
// texture cache made for the first model that loaded the image.
inline VkSamplerCreateInfo m2TextureSamplerInfo(uint8_t texFlags, uint32_t mipLevels,
                                                float mipLodBias) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.minFilter = VK_FILTER_LINEAR;
    info.magFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = (texFlags & 0x1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                         : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = (texFlags & 0x2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                         : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = 16.0f;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    info.mipLodBias = mipLodBias;
    info.minLod = 0.0f;
    info.maxLod = static_cast<float>(mipLevels > 0 ? mipLevels - 1 : 0);
    return info;
}

// NoAlphaAdd: the texture's alpha does not attenuate what is added, so a fade
// has to come through the colour (see m2.frag.glsl).
inline VkPipelineColorBlendAttachmentState m2NoAlphaAddBlendState() {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    return state;
}

} // namespace wowee::rendering
