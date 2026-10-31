#include "rendering/face_culling.hpp"

#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"

namespace wowee::rendering {

std::vector<VkDynamicState> faceCullingDynamicStates(const VkContext& ctx) {
    std::vector<VkDynamicState> states = viewportAndScissorDynamic();
    if (ctx.faceCulling()) states.push_back(VK_DYNAMIC_STATE_CULL_MODE);
    return states;
}

FaceCulling::FaceCulling(const VkContext& ctx, VkCommandBuffer cmd)
    : ctx_(ctx), cmd_(cmd), culling_(ctx.faceCulling()) {}

void FaceCulling::show(bool twoSided) {
    if (!culling_ || (known_ && twoSided == twoSided_)) return;
    ctx_.cmdSetCullMode(cmd_, twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
    known_ = true;
    twoSided_ = twoSided;
}

} // namespace wowee::rendering
