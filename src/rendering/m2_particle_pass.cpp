#include "rendering/m2_particle_pass.hpp"

#include "core/logger.hpp"
#include "rendering/particle_pool.hpp"
#include "rendering/skeleton_pool.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

/// M2ParticleQuad, and a ribbon vertex, as the shaders write them.
constexpr VkDeviceSize kQuadBytes = 31 * sizeof(float);
constexpr VkDeviceSize kRibbonVertexBytes = 9 * sizeof(float);
/// The most quads or ribbon vertices a frame's buffer grows to.
constexpr uint32_t kMostQuads = 1u << 20u;
constexpr uint32_t kBindings = 8;

/// Counters in m2_emitters.glsli: for the quads and the ribbon vertices,
/// what was taken (wanted, in all), the room and what did not fit.
struct Counters {
    uint32_t taken = 0;
    uint32_t room = 0;
    uint32_t wantedBeyond = 0;
    uint32_t ribbonTaken = 0;
    uint32_t ribbonRoom = 0;
    uint32_t ribbonWantedBeyond = 0;
};

} // namespace

void ParticleFrame::clear() noexcept {
    rows.clear();
    ribbonFirst = 0;
    owners.clear();
    blobPages.clear();
    poolPages.clear();
}

bool M2ParticlePass::initialize(VkContext& context, VkDescriptorSetLayout bonesLayout, const char* owner) {
    context_ = &context;
    owner_ = owner;
    VkDevice device = context.getDevice();

    // Binding 0: the emitter blobs' pages, 1: the particles' pages, as many
    // as are made (partially bound). 2: the rows. 3: the quads. 4: the draw
    // commands. 5: the counters. 6: what is read back. 7: the ribbon
    // vertices.
    {
        VkDescriptorSetLayoutBinding bindings[kBindings] = {};
        VkDescriptorBindingFlags bindingFlags[kBindings] = {};
        for (uint32_t b = 0; b < kBindings; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = b == 0 ? SkeletonPool::kMaxPages : b == 1 ? ParticlePool::kMaxPages : 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindingFlags[b] = b <= 1 ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        flagsInfo.bindingCount = kBindings;
        flagsInfo.pBindingFlags = bindingFlags;
        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &flagsInfo;
        ci.bindingCount = kBindings;
        ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &setLayout_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the particle pass's set layout; no particle is drawn");
            return false;
        }
    }
    {
        const VkDescriptorPoolSize size{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 2 * (SkeletonPool::kMaxPages + ParticlePool::kMaxPages + kBindings - 2)};
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 2;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &ci, nullptr, &pool_) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not create the particle pass's descriptor pool; no particle is drawn");
            return false;
        }
    }
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0,
                                   .size = sizeof(glm::vec4) + sizeof(glm::uvec4)};
    pipelineLayout_ = createPipelineLayout(device, {setLayout_, bonesLayout, bonesLayout}, {push});
    if (!pipelineLayout_) {
        LOG_ERROR(owner_, ": could not create the particle pass's pipeline layout; no particle is drawn");
        return false;
    }
    const auto build = [&](const char* path, VkPipeline& pipeline) {
        VkShaderModule shader;
        if (!shader.loadFromFile(device, path)) {
            LOG_ERROR(owner_, ": could not load ", path, "; no particle or ribbon is drawn");
            return false;
        }
        VkComputePipelineCreateInfo pipelineCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineCi.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        pipelineCi.layout = pipelineLayout_;
        const VkResult made =
            vkCreateComputePipelines(device, context.getPipelineCache(), 1, &pipelineCi, nullptr, &pipeline);
        shader.destroy();
        if (made != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not build ", path, " (", static_cast<int>(made),
                      "); no particle or ribbon is drawn");
            return false;
        }
        return true;
    };
    if (!build("assets/shaders/m2_particles.comp.spv", pipeline_) ||
        !build("assets/shaders/m2_ribbons.comp.spv", ribbonPipeline_))
        return false;
    for (auto& slot : slots_) {
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        if (context.allocateDescriptorSets(ai, &slot.set) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not allocate the particle pass's sets; no particle is drawn");
            return false;
        }
    }
    return true;
}

void M2ParticlePass::destroy() {
    if (!context_) return;
    DeferredResources retired(*context_);
    for (auto& slot : slots_) {
        for (AllocatedBuffer* buffer :
             {&slot.rows, &slot.quads, &slot.ribbonVertices, &slot.draws, &slot.counters, &slot.read})
            if (buffer->buffer) retired.buffer(*buffer);
        slot = Slot{};
    }
    retired.destroy(pipeline_);
    retired.destroy(ribbonPipeline_);
    retired.destroy(pipelineLayout_);
    retired.destroy(pool_);
    retired.destroy(setLayout_);
    context_ = nullptr;
}

bool M2ParticlePass::ensure(Slot& slot, uint32_t rows) {
    const VmaAllocator allocator = context_->getAllocator();
    DeferredResources retired(*context_);
    const auto grow = [&](AllocatedBuffer& buffer, uint32_t& room, uint32_t wanted, VkDeviceSize each,
                          VkBufferUsageFlags usage, VmaMemoryUsage memory, const char* what) {
        if (wanted <= room && buffer.buffer) return true;
        const uint32_t next = std::max({wanted, room + room / 2, 64u});
        AllocatedBuffer made = createBuffer(allocator, VkDeviceSize(next) * each, usage, memory);
        if (!made.buffer) {
            static core::LogBudget budget(4, "particle pass buffers");
            if (budget.take())
                LOG_ERROR(owner_, ": no memory for the particle pass's ", what, " (", next,
                          ") - no particle is drawn this frame");
            return false;
        }
        if (buffer.buffer) retired.buffer(buffer);
        buffer = made;
        room = next;
        slot.setCurrent = false;
        return true;
    };
    uint32_t rowRoom = slot.rowRoom;
    uint32_t drawRoom = slot.rowRoom;
    uint32_t readRoom = slot.rowRoom;
    uint32_t counterRoom = slot.counters.buffer ? 1u : 0u;
    const bool rowsMade =
        grow(slot.rows, rowRoom, rows, sizeof(ParticleRowGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VMA_MEMORY_USAGE_CPU_TO_GPU, "rows") &&
        grow(slot.draws, drawRoom, rows, sizeof(VkDrawIndirectCommand),
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
             "draw commands") &&
        grow(slot.read, readRoom, rows, 2 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VMA_MEMORY_USAGE_GPU_TO_CPU, "read back") &&
        grow(slot.counters, counterRoom, 1, sizeof(Counters),
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU,
             "counters");
    if (!rowsMade) return false;
    // The rows' three buffers grow together.
    slot.rowRoom = std::min({rowRoom, drawRoom, readRoom});
    if (slot.quadRoom < quadRoom_ || !slot.quads.buffer) {
        uint32_t quadRoom = 0;
        if (!grow(slot.quads, quadRoom, quadRoom_, kQuadBytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
                  "quads"))
            return false;
        slot.quadRoom = quadRoom;
    }
    if (slot.ribbonVertexRoom < ribbonVertexRoom_ || !slot.ribbonVertices.buffer) {
        uint32_t room = 0;
        if (!grow(slot.ribbonVertices, room, ribbonVertexRoom_, kRibbonVertexBytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
                  "ribbon vertices"))
            return false;
        slot.ribbonVertexRoom = room;
    }
    return true;
}

void M2ParticlePass::writeSet(Slot& slot, const ParticleFrame& frame) {
    std::vector<VkDescriptorBufferInfo> blobs, particles;
    std::vector<uint32_t> blobAt, particleAt;
    for (size_t p = 0; p < frame.blobPages.size() && p < SkeletonPool::kMaxPages; ++p) {
        if (!frame.blobPages[p]) continue;
        blobs.push_back({.buffer = frame.blobPages[p], .offset = 0, .range = VK_WHOLE_SIZE});
        blobAt.push_back(static_cast<uint32_t>(p));
    }
    for (size_t p = 0; p < frame.poolPages.size() && p < ParticlePool::kMaxPages; ++p) {
        if (!frame.poolPages[p]) continue;
        particles.push_back({.buffer = frame.poolPages[p], .offset = 0, .range = VK_WHOLE_SIZE});
        particleAt.push_back(static_cast<uint32_t>(p));
    }
    const VkDescriptorBufferInfo singles[] = {
        {.buffer = slot.rows.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
        {.buffer = slot.quads.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
        {.buffer = slot.draws.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
        {.buffer = slot.counters.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
        {.buffer = slot.read.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
        {.buffer = slot.ribbonVertices.buffer, .offset = 0, .range = VK_WHOLE_SIZE},
    };
    std::vector<VkWriteDescriptorSet> writes;
    const auto write = [&](uint32_t binding, uint32_t element, const VkDescriptorBufferInfo* info) {
        VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = slot.set;
        w.dstBinding = binding;
        w.dstArrayElement = element;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = info;
        writes.push_back(w);
    };
    for (size_t p = 0; p < blobs.size(); ++p) write(0, blobAt[p], &blobs[p]);
    for (size_t p = 0; p < particles.size(); ++p) write(1, particleAt[p], &particles[p]);
    for (uint32_t b = 0; b < std::size(singles); ++b) write(2 + b, 0, &singles[b]);
    vkUpdateDescriptorSets(context_->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    slot.setCurrent = true;
    slot.blobVersion = frame.blobVersion;
    slot.poolVersion = frame.poolVersion;
}

bool M2ParticlePass::record(VkCommandBuffer cmd, uint32_t slotIndex, const ParticleFrame& frame,
                            VkDescriptorSet bones, VkDescriptorSet characterBones, bool charactersPosed) {
    if (!pipeline_ || slotIndex >= 2) return false;
    Slot& slot = slots_[slotIndex];
    const VmaAllocator allocator = context_->getAllocator();

    // What this slot's last frame left: its fence has passed.
    if (!slot.readOf.empty() && slot.read.info.pMappedData && slot.counters.info.pMappedData) {
        vmaInvalidateAllocation(allocator, slot.read.allocation, 0, VK_WHOLE_SIZE);
        vmaInvalidateAllocation(allocator, slot.counters.allocation, 0, VK_WHOLE_SIZE);
        const auto* read = static_cast<const uint32_t*>(slot.read.info.pMappedData);
        {
            std::lock_guard lock(readMutex_);
            for (size_t i = 0; i < slot.readOf.size(); ++i)
                read_.push_back({.owner = slot.readOf[i], .live = read[i * 2],
                                 .reach = std::bit_cast<float>(read[i * 2 + 1])});
        }
        Counters counters{};
        std::memcpy(&counters, slot.counters.info.pMappedData, sizeof(counters));
        quadsLaidOut_.store(std::min(counters.taken, counters.room), std::memory_order_relaxed);
        if (counters.wantedBeyond > 0) {
            const uint32_t wanted = counters.taken;
            const uint32_t next = std::min(kMostQuads, std::max(quadRoom_ + quadRoom_ / 2, wanted));
            static core::LogBudget budget(8, "particle quads short");
            if (budget.take())
                LOG_WARNING(owner_, ": ", counters.wantedBeyond, " particle quads in view were not drawn (room for ",
                            counters.room, "); room for ", next, " from the next frame");
            quadRoom_ = next;
        }
        if (counters.ribbonWantedBeyond > 0) {
            const uint32_t next =
                std::min(kMostQuads, std::max(ribbonVertexRoom_ + ribbonVertexRoom_ / 2, counters.ribbonTaken));
            static core::LogBudget budget(8, "ribbon vertices short");
            if (budget.take())
                LOG_WARNING(owner_, ": ", counters.ribbonWantedBeyond,
                            " ribbon vertices in view were not drawn (room for ", counters.ribbonRoom,
                            "); room for ", next, " from the next frame");
            ribbonVertexRoom_ = next;
        }
    }
    slot.readOf.clear();

    if (frame.rows.empty()) {
        quadsLaidOut_.store(0, std::memory_order_relaxed);
        return true;
    }
    const auto rows = static_cast<uint32_t>(frame.rows.size());
    if (!ensure(slot, rows)) return false;
    std::memcpy(slot.rows.info.pMappedData, frame.rows.data(), frame.rows.size() * sizeof(ParticleRowGPU));
    vmaFlushAllocation(allocator, slot.rows.allocation, 0, VK_WHOLE_SIZE);
    if (!slot.setCurrent || slot.blobVersion != frame.blobVersion || slot.poolVersion != frame.poolVersion)
        writeSet(slot, frame);

    const Counters start{.taken = 0, .room = slot.quadRoom, .wantedBeyond = 0,
                         .ribbonTaken = 0, .ribbonRoom = slot.ribbonVertexRoom, .ribbonWantedBeyond = 0};
    vkCmdUpdateBuffer(cmd, slot.counters.buffer, 0, sizeof(start), &start);
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
    // The counters, and the particles as the frame before left them.
    barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    const VkDescriptorSet sets[] = {slot.set, bones, characterBones};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 3, sets, 0, nullptr);
    struct {
        glm::vec4 eye;
        glm::uvec4 rows;
    } push{.eye = glm::vec4(frame.eye, frame.density), .rows = glm::uvec4(0u, charactersPosed ? 1u : 0u, 0u, 0u)};
    // The particle emitters' rows, then the ribbons': a workgroup a row.
    const auto dispatch = [&](VkPipeline pipeline, uint32_t begin, uint32_t end) {
        if (end <= begin) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        constexpr uint32_t kMostGroups = 65535;
        for (uint32_t first = begin; first < end; first += kMostGroups) {
            push.rows.x = first;
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, std::min(kMostGroups, end - first), 1, 1);
        }
    };
    const uint32_t ribbonFirst = std::min(frame.ribbonFirst, rows);
    dispatch(pipeline_, 0, ribbonFirst);
    dispatch(ribbonPipeline_, ribbonFirst, rows);
    // The quads and commands for the draws, the rest for the host.
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
    slot.readOf = frame.owners;
    return true;
}

std::vector<M2ParticlePass::Read> M2ParticlePass::takeRead() {
    std::vector<Read> read;
    std::lock_guard lock(readMutex_);
    read.swap(read_);
    return read;
}

} // namespace rendering
} // namespace wowee
