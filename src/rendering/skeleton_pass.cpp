#include "rendering/skeleton_pass.hpp"

#include "core/logger.hpp"
#include "rendering/skeleton_pool.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {

void SkeletonFrame::clear() noexcept {
    requests.clear();
    levelEnds.clear();
    boneCount = 0;
    pages.clear();
    reads.clear();
}

bool SkeletonPass::initialize(VkContext& context, const char* owner) {
    context_ = &context;
    owner_ = owner;
    VkDevice device = context.getDevice();

    // The pass's set. Binding 0: the pool's pages, as many as are made
    // (partially bound). 1: the requests. 2: the bones. 3: the models.
    {
        VkDescriptorSetLayoutBinding bindings[4] = {};
        for (uint32_t b = 0; b < 4; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = b == 0 ? SkeletonPool::kMaxPages : 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        const VkDescriptorBindingFlags bindingFlags[4] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0, 0};
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        flagsInfo.bindingCount = 4;
        flagsInfo.pBindingFlags = bindingFlags;
        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &flagsInfo;
        ci.bindingCount = 4;
        ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &computeSetLayout_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the skeleton pass's set layout; nothing posed is drawn");
            return false;
        }
    }
    // The draws' set: the bones and the models.
    {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        for (uint32_t b = 0; b < 2; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 2;
        ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &drawSetLayout_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the posed bones' set layout; nothing posed is drawn");
            return false;
        }
    }
    {
        const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        .descriptorCount = 2 * (SkeletonPool::kMaxPages + 3 + 2)};
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 4;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &ci, nullptr, &pool_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the skeleton pass's descriptor pool; nothing posed is drawn");
            return false;
        }
    }

    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0,
                                   .size = sizeof(glm::mat4) + sizeof(glm::uvec4)};
    pipelineLayout_ = createPipelineLayout(device, {computeSetLayout_}, {push});
    VkShaderModule shader;
    if (!pipelineLayout_ || !shader.loadFromFile(device, "assets/shaders/skeleton.comp.spv")) {
        LOG_ERROR(owner_, ": could not load the skeleton pass; nothing posed is drawn");
        return false;
    }
    VkComputePipelineCreateInfo pipelineCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineCi.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    pipelineCi.layout = pipelineLayout_;
    const VkResult made =
        vkCreateComputePipelines(device, context.getPipelineCache(), 1, &pipelineCi, nullptr, &pipeline_);
    shader.destroy();
    if (made != VK_SUCCESS) {
        LOG_ERROR(owner_, ": could not build the skeleton pass (", static_cast<int>(made), "); nothing posed is drawn");
        return false;
    }
    for (auto& slot : slots_) {
        const VkDescriptorSetLayout layouts[2] = {computeSetLayout_, drawSetLayout_};
        VkDescriptorSet sets[2] = {};
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        if (context.allocateDescriptorSets(ai, sets) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not allocate the skeleton pass's sets; nothing posed is drawn");
            return false;
        }
        slot.computeSet = sets[0];
        slot.drawSet = sets[1];
    }
    return true;
}

void SkeletonPass::destroy() {
    if (!context_) return;
    DeferredResources retired(*context_);
    for (auto& slot : slots_) {
        for (AllocatedBuffer* buffer : {&slot.requests, &slot.bones, &slot.models, &slot.readback})
            if (buffer->buffer) retired.buffer(*buffer);
        slot = Slot{};
    }
    retired.destroy(pipeline_);
    retired.destroy(pipelineLayout_);
    retired.destroy(pool_);
    retired.destroy(computeSetLayout_);
    retired.destroy(drawSetLayout_);
    context_ = nullptr;
}

bool SkeletonPass::ensure(Slot& slot, uint32_t requests, uint32_t bones, uint32_t reads) {
    const VmaAllocator allocator = context_->getAllocator();
    DeferredResources retired(*context_);
    const auto grow = [&](AllocatedBuffer& buffer, uint32_t& room, uint32_t wanted, VkDeviceSize each,
                          VkBufferUsageFlags usage, VmaMemoryUsage memory, const char* what) {
        if (wanted <= room && buffer.buffer) return true;
        const uint32_t next = std::max({wanted, room + room / 2, 64u});
        AllocatedBuffer made = createBuffer(allocator, VkDeviceSize(next) * each, usage, memory);
        if (!made.buffer) {
            static core::LogBudget budget(4, "skeleton pass buffers");
            if (budget.take())
                LOG_ERROR(owner_, ": no memory for the skeleton pass's ", what, " (", next,
                          ") - nothing posed is drawn this frame");
            return false;
        }
        if (buffer.buffer) retired.buffer(buffer);
        buffer = made;
        room = next;
        slot.setsCurrent = false;
        return true;
    };
    return grow(slot.requests, slot.requestRoom, requests, sizeof(SkeletonRequestGPU),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "requests") &&
           grow(slot.models, slot.modelRoom, requests, sizeof(glm::mat4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_MEMORY_USAGE_GPU_ONLY, "models") &&
           grow(slot.bones, slot.boneRoom, bones, sizeof(glm::mat4),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
                "bones") &&
           grow(slot.readback, slot.readRoom, std::max(reads, 1u), sizeof(glm::mat4),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, "bones read back");
}

bool SkeletonPass::record(VkCommandBuffer cmd, uint32_t slotIndex, const SkeletonFrame& frame) {
    if (!pipeline_ || slotIndex >= 2) return false;
    Slot& slot = slots_[slotIndex];

    // What this slot's last frame copied back: its fence has passed.
    if (!slot.readOf.empty() && slot.readback.info.pMappedData) {
        vmaInvalidateAllocation(context_->getAllocator(), slot.readback.allocation, 0, VK_WHOLE_SIZE);
        const auto* matrices = static_cast<const glm::mat4*>(slot.readback.info.pMappedData);
        std::lock_guard lock(readMutex_);
        for (size_t i = 0; i < slot.readOf.size(); ++i)
            read_.push_back({.owner = slot.readOf[i].first, .bone = slot.readOf[i].second, .matrix = matrices[i]});
    }
    slot.readOf.clear();

    if (frame.requests.empty()) return true;
    const auto requestCount = static_cast<uint32_t>(frame.requests.size());
    const auto readCount = static_cast<uint32_t>(frame.reads.size());
    if (!ensure(slot, requestCount, std::max(frame.boneCount, 1u), readCount)) return false;
    std::memcpy(slot.requests.info.pMappedData, frame.requests.data(),
                frame.requests.size() * sizeof(SkeletonRequestGPU));
    vmaFlushAllocation(context_->getAllocator(), slot.requests.allocation, 0, VK_WHOLE_SIZE);

    // The sets, after the slot's fence, when a buffer or the pages changed.
    if (!slot.setsCurrent || slot.pagesVersion != frame.pagesVersion) {
        std::vector<VkDescriptorBufferInfo> pages;
        std::vector<uint32_t> pageAt;
        for (size_t p = 0; p < frame.pages.size() && p < SkeletonPool::kMaxPages; ++p) {
            if (!frame.pages[p]) continue;
            pages.push_back({.buffer = frame.pages[p], .offset = 0, .range = VK_WHOLE_SIZE});
            pageAt.push_back(static_cast<uint32_t>(p));
        }
        const VkDescriptorBufferInfo requests{.buffer = slot.requests.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo bones{.buffer = slot.bones.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo models{.buffer = slot.models.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        std::vector<VkWriteDescriptorSet> writes;
        const auto write = [&](VkDescriptorSet set, uint32_t binding, uint32_t element,
                               const VkDescriptorBufferInfo* info) {
            VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.dstArrayElement = element;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = info;
            writes.push_back(w);
        };
        for (size_t p = 0; p < pages.size(); ++p) write(slot.computeSet, 0, pageAt[p], &pages[p]);
        write(slot.computeSet, 1, 0, &requests);
        write(slot.computeSet, 2, 0, &bones);
        write(slot.computeSet, 3, 0, &models);
        write(slot.drawSet, 0, 0, &bones);
        write(slot.drawSet, 1, 0, &models);
        vkUpdateDescriptorSets(context_->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
        slot.setsCurrent = true;
        slot.pagesVersion = frame.pagesVersion;
    }

    // A dispatch a level: a request reads what the level before posed.
    const auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst,
                             VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = src;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dst;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &slot.computeSet, 0, nullptr);
    struct {
        glm::mat4 view;
        glm::uvec4 level;
    } push{.view = frame.view, .level = glm::uvec4(0)};
    uint32_t first = 0;
    for (const uint32_t end : frame.levelEnds) {
        if (end > first) {
            push.level.x = first;
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, end - first, 1, 1);
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        first = end;
    }

    // The bones the CPU reads, copied back for it to take once this slot's
    // fence has passed.
    if (readCount > 0) {
        std::vector<VkBufferCopy> regions;
        regions.reserve(readCount);
        slot.readOf.reserve(readCount);
        for (uint32_t i = 0; i < readCount; ++i) {
            const SkeletonFrame::Read& read = frame.reads[i];
            regions.push_back({.srcOffset = VkDeviceSize(read.source) * sizeof(glm::mat4),
                               .dstOffset = VkDeviceSize(i) * sizeof(glm::mat4),
                               .size = sizeof(glm::mat4)});
            slot.readOf.emplace_back(read.owner, read.bone);
        }
        vkCmdCopyBuffer(cmd, slot.bones.buffer, slot.readback.buffer, readCount, regions.data());
        barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_HOST_READ_BIT);
    }
    return true;
}

std::vector<SkeletonPass::ReadBone> SkeletonPass::takeReadBones() {
    std::vector<ReadBone> read;
    std::lock_guard lock(readMutex_);
    read.swap(read_);
    return read;
}

} // namespace rendering
} // namespace wowee
