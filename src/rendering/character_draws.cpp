#include "rendering/character_draws.hpp"

#include "foundation/logger.hpp"
#include "rendering/gpu_cull.hpp"
#include "rendering/skeleton_pool.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

constexpr uint32_t kCullBindings = 10;
constexpr uint32_t kDrawBindings = 4;
constexpr uint32_t kCasterBindings = 2;

struct CullPush {
    glm::vec4 depth{0.0f};
    glm::uvec4 mode{0u};
};


}  // namespace

void CharDrawFrame::clear() noexcept {
    entryRowCount = materialRowCount = 0;
    entrySlots.clear();
    materialSlots.clear();
    entryRows.clear();
    materialRows.clear();
    instances.clear();
    bucketStarts.clear();
    colour.clear();
    casters.clear();
    pages.clear();
    colourPlaces = casterPlaces = 0;
    cull = CharCullFrameGPU{};
}

// --- CharacterDrawLayout ---------------------------------------------------

uint32_t CharacterDrawLayout::slotOf(uint32_t id) {
    if (const auto found = slotById_.find(id); found != slotById_.end()) return found->second;
    uint32_t slot;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
        slotIds_[slot] = id;
    } else {
        slot = static_cast<uint32_t>(slotIds_.size());
        slotIds_.push_back(id);
    }
    slotById_[id] = slot;
    return slot;
}

void CharacterDrawLayout::release(List& list) {
    for (const Taken& taken : list.places) taken.bucket->places.free(taken.first, taken.count);
    list.places.clear();
    // Read as no batch by the frames from now on.
    for (uint32_t e = 0; e < list.entryCount; ++e) entries_.writeAt(list.entryBlock + e, CharEntryGPU{});
    if (list.entryCount > 0) entryBlocks_.free(list.entryBlock, list.entryCount);
    list.entryCount = 0;
    for (const uint32_t row : list.materialRows) materials_.release(*context_, row);
    list.materialRows.clear();
}

void CharacterDrawLayout::remove(uint32_t id) {
    if (const auto list = lists_.find(id); list != lists_.end()) {
        release(list->second);
        lists_.erase(list);
    }
    const auto slot = slotById_.find(id);
    if (slot == slotById_.end()) return;
    slotIds_[slot->second] = ~0u;
    freeSlots_.push_back(slot->second);
    slotById_.erase(slot);
}

void CharacterDrawLayout::clear() {
    for (auto& [id, list] : lists_) release(list);
    lists_.clear();
    slotById_.clear();
    slotIds_.clear();
    freeSlots_.clear();
}

std::optional<uint32_t> CharacterDrawLayout::take(const CharBucketKey& key, uint32_t count, bool caster,
                                                  List& list) {
    Bucket* bucket = nullptr;
    if (caster) {
        auto [at, made] = casterBuckets_.try_emplace(key.page);
        if (made) at->second.id = bucketIds_++;
        bucket = &at->second;
    } else {
        auto [at, made] = colourBuckets_.try_emplace(key);
        if (made) at->second.id = bucketIds_++;
        bucket = &at->second;
    }
    const std::optional<uint32_t> first = bucket->places.allocate(count);
    if (!first) return std::nullopt;
    bucket->size = std::max(bucket->size, *first + count);
    list.places.push_back({.bucket = bucket, .first = *first, .count = count});
    return bucket->id;
}

void CharacterDrawLayout::setList(uint32_t id, int32_t page, std::vector<CharListEntry> entries) {
    const uint32_t slot = slotOf(id);
    List& list = lists_[id];
    release(list);
    list.slot = slot;
    if (entries.empty()) return;
    const std::optional<uint32_t> block = entryBlocks_.allocate(static_cast<uint32_t>(entries.size()));
    if (!block) {
        LOG_ERROR("Character draws: no room in the entry table for instance ", id, "'s ", entries.size(),
                  " batches - it is not drawn");
        return;
    }
    list.entryBlock = *block;
    list.entryCount = static_cast<uint32_t>(entries.size());
    entryRows_ = std::max(entryRows_, *block + list.entryCount);

    // Each key's places in one block, handed to its entries in their order.
    std::map<std::pair<CharBucketKey, bool>, std::vector<std::pair<size_t, glm::uvec2 CharEntryGPU::*>>> wanted;
    for (size_t e = 0; e < entries.size(); ++e) {
        CharListEntry& entry = entries[e];
        const auto want = [&](const std::optional<CharBucketKey>& key, glm::uvec2 CharEntryGPU::*member) {
            if (!key) return;
            CharBucketKey placed = *key;
            placed.page = page;
            wanted[{placed, false}].emplace_back(e, member);
        };
        want(entry.solid, &CharEntryGPU::solid);
        want(entry.partial, &CharEntryGPU::partial);
        want(entry.fadedDepth, &CharEntryGPU::fadedDepth);
        want(entry.faded, &CharEntryGPU::faded);
        if (entry.casts) wanted[{CharBucketKey{.page = page}, true}].emplace_back(e, &CharEntryGPU::caster);
    }
    for (const auto& [key, members] : wanted) {
        const std::optional<uint32_t> bucket =
            take(key.first, static_cast<uint32_t>(members.size()), key.second, list);
        if (!bucket) {
            LOG_ERROR("Character draws: a bucket of page ", page, " is full - instance ", id,
                      " draws ", members.size(), " batches fewer");
            continue;
        }
        const uint32_t first = list.places.back().first;
        for (size_t i = 0; i < members.size(); ++i)
            entries[members[i].first].entry.*(members[i].second) = glm::uvec2(*bucket, first + static_cast<uint32_t>(i));
    }

    for (size_t e = 0; e < entries.size(); ++e) {
        CharListEntry& entry = entries[e];
        entry.entry.slot = slot;
        entry.entry.flags |= CharEntryGPU::kValid | (e == 0 ? CharEntryGPU::kFirst : 0u);
        if (entry.material) {
            const uint32_t row = materials_.place();
            materials_.write(row, *entry.material);
            list.materialRows.push_back(row);
            entry.entry.material = row;
        }
        entries_.writeAt(list.entryBlock + static_cast<uint32_t>(e), entry.entry);
    }
}

void CharacterDrawLayout::carry(uint32_t serial, uint32_t applied, CharDrawFrame& frame) {
    entries_.carry(serial, applied, frame.entrySlots, frame.entryRows);
    materials_.carry(serial, applied, frame.materialSlots, frame.materialRows);
    frame.entryRowCount = std::max(entryRows_, entries_.count());
    frame.materialRowCount = materials_.count();

    // The colour buckets one after another in drawing order - the map's -
    // and the casters' in a space of their own.
    frame.bucketStarts.assign(bucketIds_, 0u);
    uint32_t colour = 0;
    for (const auto& [key, bucket] : colourBuckets_) {
        frame.bucketStarts[bucket.id] = colour;
        if (bucket.size > 0) frame.colour.push_back({.key = key, .start = colour, .size = bucket.size});
        colour += bucket.size;
    }
    uint32_t caster = 0;
    for (const auto& [page, bucket] : casterBuckets_) {
        frame.bucketStarts[bucket.id] = caster;
        if (bucket.size > 0)
            frame.casters.push_back({.key = CharBucketKey{.page = page}, .start = caster, .size = bucket.size});
        caster += bucket.size;
    }
    frame.colourPlaces = colour;
    frame.casterPlaces = caster;
    frame.cull.counts = glm::uvec4(frame.entryRowCount, 0u, colour, caster);
}

// --- CharacterDrawPass -----------------------------------------------------

bool CharacterDrawPass::initialize(VkContext& context, const char* owner) {
    context_ = &context;
    owner_ = owner;
    VkDevice device = context.getDevice();
    entries_.initialize(context);
    materials_.initialize(context);

    const auto makeLayout = [&](uint32_t count, VkShaderStageFlags stages, int uniformAt,
                                VkDescriptorSetLayout& out) {
        std::vector<VkDescriptorSetLayoutBinding> bindings(count);
        for (uint32_t b = 0; b < count; ++b)
            bindings[b] = {.binding = b,
                           .descriptorType = static_cast<int>(b) == uniformAt ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           .descriptorCount = 1, .stageFlags = stages};
        out = createDescriptorSetLayout(device, bindings);
        return out != VK_NULL_HANDLE;
    };
    // Set 0 of the cull: the skeleton pool's pages, which hold the models'
    // animation blobs, as many as are made (partially bound).
    {
        VkDescriptorSetLayoutBinding binding{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .descriptorCount = SkeletonPool::kMaxPages,
                                             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        flagsInfo.bindingCount = 1;
        flagsInfo.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &flagsInfo;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &pageSetLayout_) != VK_SUCCESS) pageSetLayout_ = {};
    }
    // The cull's frame; the draws' (set 3 of the colour pipelines: records,
    // materials, instances, lights); the casters' (set 2 of the shadow one).
    if (!pageSetLayout_ || !makeLayout(kCullBindings, VK_SHADER_STAGE_COMPUTE_BIT, 2, cullSetLayout_) ||
        !makeLayout(kDrawBindings, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, -1, drawSetLayout_) ||
        !makeLayout(kCasterBindings, VK_SHADER_STAGE_VERTEX_BIT, -1, casterSetLayout_)) {
        LOG_ERROR(owner_, ": could not make the character cull's set layouts; no characters are drawn");
        return false;
    }
    const VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 2 * (SkeletonPool::kMaxPages + kCullBindings + kDrawBindings + kCasterBindings)},
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2}};
    VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets = 8;
    poolCi.poolSizeCount = 2;
    poolCi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCi, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR(owner_, ": could not make the character cull's pool; no characters are drawn");
        return false;
    }
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(CullPush)};
    pipelineLayout_ = createPipelineLayout(device, {pageSetLayout_, cullSetLayout_}, {push});
    VkShaderModule shader;
    if (!pipelineLayout_ || !shader.loadFromFile(device, "assets/shaders/char_draws.comp.spv")) {
        LOG_ERROR(owner_, ": could not load char_draws.comp; no characters are drawn");
        return false;
    }
    VkComputePipelineCreateInfo pipelineCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineCi.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    pipelineCi.layout = pipelineLayout_;
    const VkResult made =
        vkCreateComputePipelines(device, context.getPipelineCache(), 1, &pipelineCi, nullptr, &pipeline_);
    shader.destroy();
    if (made != VK_SUCCESS) {
        LOG_ERROR(owner_, ": could not build the character cull (", static_cast<int>(made),
                  "); no characters are drawn");
        return false;
    }
    noLights_ = createBuffer(context.getAllocator(), 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY);
    if (!noLights_.buffer) {
        LOG_ERROR(owner_, ": no memory for the character cull's empty light list; no characters are drawn");
        return false;
    }
    for (Slot& slot : slots_) {
        const VkDescriptorSetLayout layouts[4] = {pageSetLayout_, cullSetLayout_, drawSetLayout_, casterSetLayout_};
        VkDescriptorSet sets[4] = {};
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 4;
        ai.pSetLayouts = layouts;
        if (context.allocateDescriptorSets(ai, sets) != VK_SUCCESS) {
            LOG_ERROR(owner_, ": could not allocate the character cull's sets; no characters are drawn");
            return false;
        }
        slot.pageSet = sets[0];
        slot.cullSet = sets[1];
        slot.drawSet = sets[2];
        slot.casterSet = sets[3];
    }
    return true;
}

void CharacterDrawPass::destroy() {
    if (!context_) return;
    DeferredResources retired(*context_);
    for (Slot& slot : slots_) {
        for (AllocatedBuffer* buffer : {&slot.instances, &slot.frame, &slot.bucketStarts, &slot.commands,
                                        &slot.draws, &slot.casterRecords, &slot.instanceLights, &slot.stats})
            if (buffer->buffer) retired.buffer(*buffer);
        slot = Slot{};
    }
    if (noLights_.buffer) retired.buffer(noLights_);
    retired.destroy(pipeline_);
    retired.destroy(pipelineLayout_);
    retired.destroy(pool_);
    for (VkDescriptorSetLayout* layout : {&pageSetLayout_, &cullSetLayout_, &drawSetLayout_, &casterSetLayout_})
        retired.destroy(*layout);
    entries_.destroy();
    materials_.destroy();
    context_ = nullptr;
}

bool CharacterDrawPass::ensure(Slot& slot, const CharDrawFrame& frame) {
    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bool replaced = false;
    const auto grow = [&](AllocatedBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, VmaMemoryUsage memory,
                          const char* what) {
        return growCullBuffer(*context_, buffer, bytes, usage, memory, owner_, what, replaced);
    };
    const auto instances = static_cast<VkDeviceSize>(frame.instances.size());
    const VkDeviceSize commands = VkDeviceSize(frame.colourPlaces + kCharShadowMaps * frame.casterPlaces) *
                                  sizeof(VkDrawIndexedIndirectCommand);
    const bool grown =
        grow(slot.instances, instances * sizeof(CharInstanceGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VMA_MEMORY_USAGE_CPU_TO_GPU, "instances") &&
        grow(slot.frame, sizeof(CharCullFrameGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
             "frame view") &&
        grow(slot.bucketStarts, frame.bucketStarts.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VMA_MEMORY_USAGE_CPU_TO_GPU, "bucket starts") &&
        grow(slot.commands, commands, kStorage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
             "draw commands") &&
        grow(slot.draws, VkDeviceSize(frame.colourPlaces) * sizeof(CharDrawGPU), kStorage,
             VMA_MEMORY_USAGE_GPU_ONLY, "draw records") &&
        grow(slot.casterRecords, VkDeviceSize(frame.casterPlaces) * sizeof(glm::uvec4), kStorage,
             VMA_MEMORY_USAGE_GPU_ONLY, "caster records") &&
        grow(slot.instanceLights, instances * sizeof(glm::uvec4), kStorage, VMA_MEMORY_USAGE_GPU_ONLY,
             "instance lights") &&
        grow(slot.stats, sizeof(CharCullStatsGPU), kStorage, VMA_MEMORY_USAGE_GPU_TO_CPU, "statistics");
    if (replaced) ++slot.version;
    return grown;
}

void CharacterDrawPass::writeSets(Slot& slot) {
    const auto write = [&](VkDescriptorSet set, std::initializer_list<VkDescriptorBufferInfo> infos, int uniformAt) {
        std::vector<VkWriteDescriptorSet> writes(infos.size());
        uint32_t b = 0;
        for (const VkDescriptorBufferInfo& info : infos) {
            writes[b] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = static_cast<int>(b) == uniformAt ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &info;
            ++b;
        }
        vkUpdateDescriptorSets(context_->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };
    const VkDescriptorBufferInfo entries{entries_.buffer(), 0, entries_.bytes()};
    const VkDescriptorBufferInfo materials{materials_.buffer(), 0, materials_.bytes()};
    const auto whole = [](const AllocatedBuffer& buffer) { return VkDescriptorBufferInfo{buffer.buffer, 0, VK_WHOLE_SIZE}; };
    write(slot.cullSet,
          {entries, whole(slot.instances), {slot.frame.buffer, 0, sizeof(CharCullFrameGPU)}, whole(slot.bucketStarts),
           whole(slot.commands), whole(slot.draws), whole(slot.casterRecords), whole(slot.instanceLights),
           whole(slot.stats), {slot.lights, 0, VK_WHOLE_SIZE}},
          2);
    write(slot.drawSet, {whole(slot.draws), materials, whole(slot.instances), whole(slot.instanceLights)}, -1);
    write(slot.casterSet, {whole(slot.casterRecords), whole(slot.instances)}, -1);
    slot.setVersion = slot.version;
}


bool CharacterDrawPass::prepare(VkCommandBuffer cmd, uint32_t slotIndex, const CharDrawFrame& frame,
                                const std::vector<::VkBuffer>& pages, uint32_t pagesVersion, ::VkBuffer lights,
                                uint32_t lightCount) {
    if (slotIndex >= 2) return false;
    Slot& slot = slots_[slotIndex];
    // What this slot's last frame counted: its fence was waited for.
    if (std::exchange(slot.counting, false)) readCullCounts(*context_, slot.stats, &counted_, sizeof(counted_));
    slot.ready = false;
    slot.shadowMaps = 0;
    if (!pipeline_) return false;
    if (!lights) {
        lights = noLights_.buffer;
        lightCount = 0;
    }
    if (!entries_.update(cmd, slotIndex, frame.entryRowCount, frame.entrySlots, frame.entryRows.data()) ||
        !materials_.update(cmd, slotIndex, frame.materialRowCount, frame.materialSlots, frame.materialRows.data())) {
        return false;
    }
    if (frame.entryRowCount == 0 || frame.materialRowCount == 0 || frame.instances.empty() ||
        frame.colourPlaces + frame.casterPlaces == 0) {
        return false;
    }
    if (!ensure(slot, frame)) return false;
    const uint32_t tables = entries_.version() * 2654435761u ^ materials_.version();
    if (slot.tablesVersion != tables || slot.lights != lights) {
        slot.tablesVersion = tables;
        slot.lights = lights;
        ++slot.version;
    }
    if (slot.setVersion != slot.version) writeSets(slot);
    if (slot.pagesVersion != pagesVersion) {
        writePoolPageSet(context_->getDevice(), slot.pageSet, 0, pages);
        slot.pagesVersion = pagesVersion;
    }

    CharCullFrameGPU view = frame.cull;
    view.counts.y = lightCount;
    std::memcpy(slot.frame.info.pMappedData, &view, sizeof(view));
    std::memcpy(slot.instances.info.pMappedData, frame.instances.data(),
                frame.instances.size() * sizeof(CharInstanceGPU));
    std::memcpy(slot.bucketStarts.info.pMappedData, frame.bucketStarts.data(),
                frame.bucketStarts.size() * sizeof(uint32_t));
    for (AllocatedBuffer* buffer : {&slot.frame, &slot.instances, &slot.bucketStarts})
        vmaFlushAllocation(context_->getAllocator(), buffer->allocation, 0, VK_WHOLE_SIZE);
    slot.entries = frame.entryRowCount;
    slot.colourPlaces = frame.colourPlaces;
    slot.casterPlaces = frame.casterPlaces;

    clearCullTargets(cmd, slot.commands.buffer, slot.stats.buffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    const VkDescriptorSet sets[2] = {slot.pageSet, slot.cullSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, sets, 0, nullptr);
    const CullPush push{};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (frame.entryRowCount + 63) / 64, 1, 1);
    finishColourCull(cmd);
    slot.ready = true;
    slot.counting = true;
    return true;
}

std::optional<uint32_t> CharacterDrawPass::cullShadowMap(VkCommandBuffer cmd, uint32_t slotIndex,
                                                         const glm::vec3& centre, float reach) {
    if (slotIndex >= 2 || !slots_[slotIndex].ready) return std::nullopt;
    Slot& slot = slots_[slotIndex];
    if (slot.casterPlaces == 0) return std::nullopt;
    if (slot.shadowMaps >= kCharShadowMaps) {
        static core::LogBudget budget(4, "character shadow maps");
        if (budget.take())
            LOG_WARNING(owner_, ": a frame asked for more than ", kCharShadowMaps,
                        " shadow maps - characters cast nothing into the rest");
        return std::nullopt;
    }
    const uint32_t map = slot.shadowMaps++;
    beginShadowCull(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    const VkDescriptorSet sets[2] = {slot.pageSet, slot.cullSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, sets, 0, nullptr);
    const CullPush push{.depth = glm::vec4(centre, reach), .mode = glm::uvec4(1u, map, 0u, 0u)};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (slot.entries + 63) / 64, 1, 1);
    finishShadowCull(cmd);
    return map;
}

} // namespace rendering
} // namespace wowee
