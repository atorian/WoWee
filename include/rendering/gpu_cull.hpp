#pragma once

// What every cull on the GPU that writes indirect draws into places does
// around its dispatch (wmo_draws.hpp, character_draws.hpp): its frame slot's
// buffers grown as the scene does, its counts read back once the slot's
// fence is passed, its places emptied and its counts zeroed before it runs,
// and its writes made visible to the draws that read them.

#include "rendering/vk_utils.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstddef>

namespace wowee {
namespace rendering {

class VkContext;

/// The stages a draw reads a cull's output in: its indirect arguments, its
/// records in the vertex and fragment shaders.
inline constexpr VkPipelineStageFlags2 kCullDrawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

/// A barrier between two stages' accesses of every buffer.
void cullBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst,
                 VkAccessFlags2 dstAccess);

/// `buffer` at least `bytes` large (256 at least): when it is not, a new one
/// half again what is wanted takes its place and the old one goes after the
/// frames in flight, and `replaced` is set. False, said with `owner` and
/// `what`, when there is no memory: the buffer stays as it was.
bool growCullBuffer(VkContext& context, AllocatedBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                    VmaMemoryUsage memory, const char* owner, const char* what, bool& replaced);

/// What a slot's last cull counted into its mapped `stats`, once its fence
/// is passed.
void readCullCounts(VkContext& context, const AllocatedBuffer& stats, void* out, size_t bytes);

/// After the slot's earlier frame's draws and culls: every place in
/// `commands` empty and every count in `stats` zero, before the cull runs.
void clearCullTargets(VkCommandBuffer cmd, ::VkBuffer commands, ::VkBuffer stats);

/// The colour cull's writes made visible to the draws and to the shadow
/// maps' culls after it.
void finishColourCull(VkCommandBuffer cmd);

/// Around a shadow map's cull: after the maps drawn before it read the
/// records it rewrites, and before its own draws read what it wrote.
void beginShadowCull(VkCommandBuffer cmd);
void finishShadowCull(VkCommandBuffer cmd);

} // namespace rendering
} // namespace wowee
