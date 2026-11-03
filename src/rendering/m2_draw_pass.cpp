#include "rendering/m2_draw_pass.hpp"

#include "core/logger.hpp"
#include "rendering/skeleton_pool.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

constexpr uint32_t kFrameBindings = 7;

}  // namespace

void M2DrawFrame::clear() noexcept {
    groups.clear();
    offsets.clear();
    clocks.clear();
    skyOpacity.clear();
    pages.clear();
    pagesVersion = 0;
}

bool M2DrawPass::initialize(VkContext& context, const char* owner) {
    context_ = &context;
    owner_ = owner;
    VkDevice device = context.getDevice();

    // Set 0: the pool's pages, as many as are made (partially bound).
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = SkeletonPool::kMaxPages;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        flagsInfo.bindingCount = 1;
        flagsInfo.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &flagsInfo;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &pageSetLayout_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the draw pass's page set layout; no models are drawn");
            return false;
        }
    }
    // Set 1: the groups, their offsets, the clocks, the sky's opacity, and
    // what is written - the records, the commands, the draws' material rows.
    {
        VkDescriptorSetLayoutBinding bindings[kFrameBindings] = {};
        for (uint32_t b = 0; b < kFrameBindings; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = kFrameBindings;
        ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &frameSetLayout_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the draw pass's frame set layout; no models are drawn");
            return false;
        }
    }
    {
        const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        .descriptorCount = 2 * (SkeletonPool::kMaxPages + kFrameBindings)};
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 4;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &ci, nullptr, &pool_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the draw pass's descriptor pool; no models are drawn");
            return false;
        }
    }

    pipelineLayout_ = createPipelineLayout(device, {pageSetLayout_, frameSetLayout_}, {});
    VkShaderModule shader;
    if (!pipelineLayout_ || !shader.loadFromFile(device, "assets/shaders/m2_draws.comp.spv")) {
        LOG_ERROR(owner_, ": could not load the draw pass; no models are drawn");
        return false;
    }
    VkComputePipelineCreateInfo pipelineCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineCi.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    pipelineCi.layout = pipelineLayout_;
    const VkResult made =
        vkCreateComputePipelines(device, context.getPipelineCache(), 1, &pipelineCi, nullptr, &pipeline_);
    shader.destroy();
    if (made != VK_SUCCESS) {
        LOG_ERROR(owner_, ": could not build the draw pass (", static_cast<int>(made), "); no models are drawn");
        return false;
    }
    for (auto& slot : slots_) {
        const VkDescriptorSetLayout layouts[2] = {pageSetLayout_, frameSetLayout_};
        VkDescriptorSet sets[2] = {};
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        if (context.allocateDescriptorSets(ai, sets) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not allocate the draw pass's sets; no models are drawn");
            return false;
        }
        slot.pageSet = sets[0];
        slot.frameSet = sets[1];
    }
    return true;
}

void M2DrawPass::destroy() {
    if (!context_) return;
    DeferredResources retired(*context_);
    for (auto& slot : slots_) {
        for (AllocatedBuffer* buffer : {&slot.groups, &slot.offsets, &slot.clocks, &slot.skyOpacity})
            if (buffer->buffer) retired.buffer(*buffer);
        slot = Slot{};
    }
    retired.destroy(pipeline_);
    retired.destroy(pipelineLayout_);
    retired.destroy(pool_);
    retired.destroy(pageSetLayout_);
    retired.destroy(frameSetLayout_);
    context_ = nullptr;
}

bool M2DrawPass::ensure(Slot& slot, const M2DrawFrame& frame) {
    const VmaAllocator allocator = context_->getAllocator();
    DeferredResources retired(*context_);
    const auto grow = [&](AllocatedBuffer& buffer, uint32_t& room, size_t wanted, VkDeviceSize each,
                          const char* what) {
        if (wanted <= room && buffer.buffer) return true;
        const auto next = static_cast<uint32_t>(std::max<size_t>({wanted, room + room / 2, 64}));
        AllocatedBuffer made = createBuffer(allocator, VkDeviceSize(next) * each, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (!made.buffer || !made.info.pMappedData) {
            destroyBuffer(allocator, made);
            static core::LogBudget budget(4, "M2 draw pass buffers");
            if (budget.take())
                LOG_ERROR(owner_, ": no memory for the draw pass's ", what, " (", next,
                          ") - no models are drawn this frame");
            return false;
        }
        if (buffer.buffer) retired.buffer(buffer);
        buffer = made;
        room = next;
        slot.frameSetCurrent = false;
        return true;
    };
    return grow(slot.groups, slot.groupRoom, frame.groups.size(), sizeof(M2DrawGroupGPU), "groups") &&
           grow(slot.offsets, slot.offsetRoom, frame.offsets.size(), sizeof(uint32_t), "bucket offsets") &&
           grow(slot.clocks, slot.clockRoom, frame.clocks.size(), sizeof(glm::vec4), "instance clocks") &&
           grow(slot.skyOpacity, slot.opacityRoom, frame.skyOpacity.size(), sizeof(float), "sky opacity");
}

void M2DrawPass::writePageSet(Slot& slot, const std::vector<::VkBuffer>& framePages, uint32_t pagesVersion) {
    std::vector<VkDescriptorBufferInfo> pages;
    std::vector<VkWriteDescriptorSet> writes;
    pages.reserve(framePages.size());
    for (size_t p = 0; p < framePages.size() && p < SkeletonPool::kMaxPages; ++p) {
        if (!framePages[p]) continue;
        pages.push_back({.buffer = framePages[p], .offset = 0, .range = VK_WHOLE_SIZE});
        VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = slot.pageSet;
        w.dstBinding = 0;
        w.dstArrayElement = static_cast<uint32_t>(p);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes.push_back(w);
    }
    for (size_t i = 0; i < writes.size(); ++i) writes[i].pBufferInfo = &pages[i];
    if (!writes.empty())
        vkUpdateDescriptorSets(context_->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    slot.pagesVersion = pagesVersion;
}

void M2DrawPass::writeFrameSet(Slot& slot, const Targets& targets) {
    const ::VkBuffer buffers[kFrameBindings] = {slot.groups.buffer, slot.offsets.buffer, slot.clocks.buffer,
                                                slot.skyOpacity.buffer, targets.instances, targets.commands,
                                                targets.drawMaterials};
    VkDescriptorBufferInfo infos[kFrameBindings] = {};
    VkWriteDescriptorSet writes[kFrameBindings] = {};
    for (uint32_t b = 0; b < kFrameBindings; ++b) {
        infos[b] = {.buffer = buffers[b], .offset = 0, .range = VK_WHOLE_SIZE};
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = slot.frameSet;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(context_->getDevice(), kFrameBindings, writes, 0, nullptr);
    slot.targets = targets;
    slot.frameSetCurrent = true;
}

bool M2DrawPass::record(VkCommandBuffer cmd, uint32_t slotIndex, const M2DrawFrame& frame, const Targets& targets) {
    if (!pipeline_ || slotIndex >= 2) return false;
    if (frame.groups.empty()) return true;
    Slot& slot = slots_[slotIndex];
    if (!ensure(slot, frame)) return false;
    const auto copy = [&](AllocatedBuffer& into, const void* data, size_t bytes) {
        if (bytes == 0) return;
        std::memcpy(into.info.pMappedData, data, bytes);
        vmaFlushAllocation(context_->getAllocator(), into.allocation, 0, VK_WHOLE_SIZE);
    };
    copy(slot.groups, frame.groups.data(), frame.groups.size() * sizeof(M2DrawGroupGPU));
    copy(slot.offsets, frame.offsets.data(), frame.offsets.size() * sizeof(uint32_t));
    copy(slot.clocks, frame.clocks.data(), frame.clocks.size() * sizeof(glm::vec4));
    copy(slot.skyOpacity, frame.skyOpacity.data(), frame.skyOpacity.size() * sizeof(float));

    // The sets, after the slot's fence, when a buffer or the pages changed.
    if (slot.pagesVersion != frame.pagesVersion) writePageSet(slot, frame.pages, frame.pagesVersion);
    if (!slot.frameSetCurrent || slot.targets.instances != targets.instances ||
        slot.targets.commands != targets.commands || slot.targets.drawMaterials != targets.drawMaterials)
        writeFrameSet(slot, targets);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    const VkDescriptorSet sets[2] = {slot.pageSet, slot.frameSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, sets, 0, nullptr);
    vkCmdDispatch(cmd, static_cast<uint32_t>(frame.groups.size()), 1, 1);

    // The draws read the commands as indirect arguments, the records and
    // material rows in the vertex shader.
    VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
    return true;
}

} // namespace rendering
} // namespace wowee
