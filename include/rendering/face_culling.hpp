#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace wowee::rendering {

class VkContext;

/// The dynamic states of a pipeline whose batches choose the faces they show:
/// the viewport and scissor every pipeline has, and the cull mode while
/// VkContext::faceCulling is on.
[[nodiscard]] std::vector<VkDynamicState> faceCullingDynamicStates(const VkContext& ctx);

/// The faces the batches being recorded into one command buffer show.
///
/// A material without the two-sided flag (M2 0x04, WMO F_UNCULLED) shows its
/// front faces only, as in the client; a two-sided one shows both. The cull
/// mode is command-buffer state, set only when it changes. Binding a pipeline
/// that holds it static replaces it, so after every bind the recorder says so
/// and the next batch sets it again.
class FaceCulling {
public:
    FaceCulling(const VkContext& ctx, VkCommandBuffer cmd);

    /// A pipeline was bound.
    void pipelineBound() { known_ = false; }

    /// Before a batch's draws.
    void show(bool twoSided);

private:
    const VkContext& ctx_;
    VkCommandBuffer cmd_;
    bool culling_;
    bool known_ = false;
    bool twoSided_ = false;
};

} // namespace wowee::rendering
