#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace wowee::rendering {

// -1 means camera-dependent environment coordinates, not UV0.
inline int8_t m2TextureCoordSet(const std::vector<int16_t>& lookup, uint16_t combo) {
    if (combo >= lookup.size()) return 0;
    const int16_t mapping = lookup[combo];
    return mapping == -1 || mapping == 1 ? static_cast<int8_t>(mapping) : 0;
}

// Shader 0 with two stages and alpha-add blending combines RGBA from
// both stages. This is shared by sky auroras and the foam on fountains.
inline bool m2UsesSecondTextureMask(uint16_t shader, uint16_t textureCount,
                                    uint16_t blendMode) {
    return shader == 0 && textureCount == 2 && blendMode == 4;
}

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
