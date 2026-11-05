#include "rendering/wmo_draws.hpp"

#include "foundation/logger.hpp"
#include "rendering/gpu_cull.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

constexpr uint32_t kCullBindings = 12;

struct CullPush {
    glm::vec4 depth{0.0f};
    glm::uvec4 mode{0u};
};

bool sameShape(const std::vector<WmoGroupDraws>& a, const std::vector<WmoGroupDraws>& b) {
    if (a.size() != b.size()) return false;
    for (size_t g = 0; g < a.size(); ++g) {
        if (a[g].page != b[g].page || a[g].kindCounts != b[g].kindCounts || a[g].casterCount != b[g].casterCount ||
            a[g].ranges.size() != b[g].ranges.size())
            return false;
        for (size_t r = 0; r < a[g].ranges.size(); ++r)
            if (a[g].ranges[r].place != b[g].ranges[r].place || a[g].ranges[r].caster != b[g].ranges[r].caster)
                return false;
    }
    return true;
}

}  // namespace

void WmoDrawFrame::clear() noexcept {
    instanceRowCount = groupRowCount = rangeRowCount = 0;
    instanceSlots.clear();
    groupSlots.clear();
    rangeSlots.clear();
    instanceRows.clear();
    groupRows.clear();
    rangeRows.clear();
    bucketStarts.clear();
    bucketSizes.clear();
    pages.clear();
    colourPlaces = casterPlaces = 0;
    portalCulled.clear();
    cull = WmoCullFrameGPU{};
}

// --- WmoDrawLayout ---------------------------------------------------------

bool WmoDrawLayout::setModel(uint32_t model, std::vector<WmoGroupDraws> groups) {
    auto found = models_.find(model);
    if (found != models_.end() && sameShape(found->second.groups, groups)) {
        // The same draws in the same places: only what they draw with moved.
        Model& kept = found->second;
        kept.groups = std::move(groups);
        for (size_t g = 0; g < kept.groups.size(); ++g)
            for (size_t r = 0; r < kept.groups[g].ranges.size(); ++r)
                ranges_.writeAt(kept.rangeFirst[g] + static_cast<uint32_t>(r), kept.groups[g].ranges[r]);
        return false;
    }
    if (found != models_.end()) removeModel(model);

    Model made;
    made.groups = std::move(groups);
    made.serial = ++modelSerial_;
    for (const auto& group : made.groups) made.rangeCount += static_cast<uint32_t>(group.ranges.size());
    if (made.rangeCount > 0) {
        const std::optional<uint32_t> block = rangeBlocks_.allocate(made.rangeCount);
        if (!block) {
            LOG_ERROR("WMO draws: no room in the range table for model ", model, "'s ", made.rangeCount,
                      " ranges - the model is not drawn");
            made.groups.clear();
            made.rangeCount = 0;
        } else {
            made.rangeBlock = *block;
            rangeRows_ = std::max(rangeRows_, *block + made.rangeCount);
        }
    }
    uint32_t next = made.rangeBlock;
    made.rangeFirst.reserve(made.groups.size());
    for (const auto& group : made.groups) {
        made.rangeFirst.push_back(next);
        for (const WmoRangeGPU& range : group.ranges) ranges_.writeAt(next++, range);
    }
    models_[model] = std::move(made);
    return true;
}

void WmoDrawLayout::removeModel(uint32_t model) {
    const auto found = models_.find(model);
    if (found == models_.end()) return;
    // Its instances are placed again when they next are, without it.
    if (found->second.rangeCount > 0) rangeBlocks_.free(found->second.rangeBlock, found->second.rangeCount);
    models_.erase(found);
}

WmoDrawLayout::Bucket& WmoDrawLayout::colourBucket(int32_t page, uint32_t kind) {
    const size_t at = static_cast<size_t>(page) * kWmoColourKinds + kind;
    if (colourBuckets_.size() <= at) colourBuckets_.resize((static_cast<size_t>(page) + 1) * kWmoColourKinds);
    return colourBuckets_[at];
}

WmoDrawLayout::Bucket& WmoDrawLayout::casterBucket(int32_t page) {
    if (casterBuckets_.size() <= static_cast<size_t>(page)) casterBuckets_.resize(static_cast<size_t>(page) + 1);
    return casterBuckets_[static_cast<size_t>(page)];
}

std::optional<uint32_t> WmoDrawLayout::take(Bucket& bucket, uint32_t count) {
    if (count == 0) return 0u;
    const std::optional<uint32_t> first = bucket.places.allocate(count);
    if (first) bucket.size = std::max(bucket.size, *first + count);
    return first;
}

void WmoDrawLayout::unplace(Instance& instance) {
    for (PlacedGroup& placed : instance.groups) {
        if (placed.row == GpuRowStore<WmoGroupGPU>::kNoRow) continue;
        // Read as no group by the frames from now on, then given out again
        // after the ones in flight.
        groups_.write(placed.row, WmoGroupGPU{});
        groups_.release(*context_, placed.row);
        for (uint32_t kind = 0; kind < kWmoColourKinds; ++kind)
            if (placed.counts[kind] > 0)
                colourBucket(placed.page, kind).places.free(placed.colour[kind], placed.counts[kind]);
        if (placed.casterCount > 0) casterBucket(placed.page).places.free(placed.caster, placed.casterCount);
    }
    instance.groups.clear();
    instance.rows.clear();
    instance.placed = false;
}

void WmoDrawLayout::place(uint32_t slot, const Placement& placement) {
    if (instances_.size() <= slot) instances_.resize(slot + 1);
    Instance& instance = instances_[slot];
    instanceRows_.writeAt(slot, WmoInstanceGPU{.model = placement.matrix,
                                               .state = glm::uvec4(placement.shown ? 1u : 0u, 0u, 0u, 0u)});
    const auto found = models_.find(placement.model);
    if (found == models_.end()) {
        unplace(instance);
        instance.model = placement.model;
        return;
    }
    const Model& model = found->second;
    if (!instance.placed || instance.model != placement.model || instance.modelSerial != model.serial) {
        unplace(instance);
        instance.model = placement.model;
        instance.modelSerial = model.serial;
        instance.placed = true;
        instance.groups.resize(model.groups.size());
        instance.rows.assign(model.groups.size(), GpuRowStore<WmoGroupGPU>::kNoRow);
        for (size_t g = 0; g < model.groups.size(); ++g) {
            const WmoGroupDraws& draws = model.groups[g];
            if (draws.page < 0 || draws.ranges.empty()) continue;
            PlacedGroup placed;
            placed.page = draws.page;
            bool room = true;
            for (uint32_t kind = 0; kind < kWmoColourKinds && room; ++kind) {
                const std::optional<uint32_t> first = take(colourBucket(draws.page, kind), draws.kindCounts[kind]);
                if (!first) {
                    room = false;
                    break;
                }
                placed.colour[kind] = *first;
                placed.counts[kind] = draws.kindCounts[kind];
            }
            const std::optional<uint32_t> caster = room ? take(casterBucket(draws.page), draws.casterCount)
                                                        : std::nullopt;
            if (!caster) {
                for (uint32_t kind = 0; kind < kWmoColourKinds; ++kind)
                    if (placed.counts[kind] > 0)
                        colourBucket(draws.page, kind).places.free(placed.colour[kind], placed.counts[kind]);
                LOG_ERROR("WMO draws: no places left on geometry page ", draws.page, " for group ", g,
                          " of model ", placement.model, " - the group is not drawn");
                continue;
            }
            placed.caster = *caster;
            placed.casterCount = draws.casterCount;
            placed.row = groups_.place();
            instance.groups[g] = placed;
            instance.rows[g] = placed.row;
        }
    }
    writeGroups(slot, instance, model, placement);
}

void WmoDrawLayout::writeGroups(uint32_t slot, Instance& instance, const Model& model, const Placement& placement) {
    for (size_t g = 0; g < instance.groups.size(); ++g) {
        const PlacedGroup& placed = instance.groups[g];
        if (placed.row == GpuRowStore<WmoGroupGPU>::kNoRow) continue;
        const WmoGroupDraws& draws = model.groups[g];
        WmoGroupGPU row;
        glm::vec3 boxMin(0.0f), boxMax(0.0f);
        if (g < placement.groupBounds.size()) {
            boxMin = placement.groupBounds[g].first;
            boxMax = placement.groupBounds[g].second;
        }
        row.worldMin = glm::vec4(boxMin, draws.swayReach * placement.scale);
        row.worldMax = glm::vec4(boxMax, 0.0f);
        row.localCentre = glm::vec4(draws.localCentre, 0.0f);
        const uint32_t flags = WmoGroupGPU::kValid | (draws.shell ? WmoGroupGPU::kShell : 0u) |
                               (draws.antiportal ? WmoGroupGPU::kAntiportal : 0u);
        row.refs = glm::uvec4(slot, model.rangeFirst[g], static_cast<uint32_t>(draws.ranges.size()), flags);
        row.slots0 = glm::uvec4(placed.colour[0], placed.colour[1], placed.colour[2], placed.colour[3]);
        row.slots1 = glm::uvec4(placed.colour[4], placed.colour[5], placed.caster, static_cast<uint32_t>(placed.page));
        groups_.write(placed.row, row);
    }
}

void WmoDrawLayout::free(uint32_t slot) {
    if (slot >= instances_.size()) return;
    unplace(instances_[slot]);
    instances_[slot] = Instance{};
    instanceRows_.writeAt(slot, WmoInstanceGPU{});
}

void WmoDrawLayout::clearInstances() {
    for (uint32_t slot = 0; slot < instances_.size(); ++slot) free(slot);
}

std::span<const uint32_t> WmoDrawLayout::groupRows(uint32_t slot) const {
    if (slot >= instances_.size()) return {};
    return instances_[slot].rows;
}

void WmoDrawLayout::carry(uint32_t serial, uint32_t applied, WmoDrawFrame& frame) {
    instanceRows_.carry(serial, applied, frame.instanceSlots, frame.instanceRows);
    groups_.carry(serial, applied, frame.groupSlots, frame.groupRows);
    ranges_.carry(serial, applied, frame.rangeSlots, frame.rangeRows);
    frame.instanceRowCount = instanceRows_.count();
    frame.groupRowCount = groups_.count();
    frame.rangeRowCount = std::max(rangeRows_, ranges_.count());

    // The colour buckets one after another, page by page; the casters' in a
    // space of their own.
    const size_t pages = std::max(colourBuckets_.size() / kWmoColourKinds, casterBuckets_.size());
    frame.bucketStarts.assign(pages * (kWmoColourKinds + 1), 0u);
    frame.bucketSizes.assign(pages * (kWmoColourKinds + 1), 0u);
    uint32_t colour = 0;
    for (size_t b = 0; b < pages * kWmoColourKinds; ++b) {
        const uint32_t size = b < colourBuckets_.size() ? colourBuckets_[b].size : 0u;
        frame.bucketStarts[b] = colour;
        frame.bucketSizes[b] = size;
        colour += size;
    }
    uint32_t caster = 0;
    for (size_t p = 0; p < pages; ++p) {
        const uint32_t size = p < casterBuckets_.size() ? casterBuckets_[p].size : 0u;
        frame.bucketStarts[pages * kWmoColourKinds + p] = caster;
        frame.bucketSizes[pages * kWmoColourKinds + p] = size;
        caster += size;
    }
    frame.colourPlaces = colour;
    frame.casterPlaces = caster;
    frame.cull.counts = glm::uvec4(frame.groupRowCount, 0u, colour, caster);
    frame.cull.shape.x = static_cast<uint32_t>(pages);
}

void WmoDrawLayout::reset() {
    models_.clear();
    instances_.clear();
    colourBuckets_.clear();
    casterBuckets_.clear();
    rangeBlocks_ = RangeAllocator(kMaxPlaces);
    rangeRows_ = 0;
    instanceRows_.reset();
    groups_.reset();
    ranges_.reset();
}

// --- WmoDrawPass -----------------------------------------------------------

bool WmoDrawPass::initialize(VkContext& context) {
    context_ = &context;
    VkDevice device = context.getDevice();
    instances_.initialize(context);
    groups_.initialize(context);
    ranges_.initialize(context);

    // The tables, the frame's view, its buckets and portal verdict, what is
    // written - commands, records, lights, counts - and the frame's lights.
    VkDescriptorSetLayoutBinding bindings[kCullBindings] = {};
    for (uint32_t b = 0; b < kCullBindings; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = b == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCi.bindingCount = kCullBindings;
    layoutCi.pBindings = bindings;
    const VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 * (kCullBindings - 1)},
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2}};
    VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets = 2;
    poolCi.poolSizeCount = 2;
    poolCi.pPoolSizes = sizes;
    if (vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &setLayout_) != VK_SUCCESS ||
        vkCreateDescriptorPool(device, &poolCi, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR("WMO cull: could not make its set layout or pool; no buildings are drawn");
        return false;
    }
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(CullPush)};
    pipelineLayout_ = createPipelineLayout(device, {setLayout_}, {push});
    VkShaderModule shader;
    if (!pipelineLayout_ || !shader.loadFromFile(device, "assets/shaders/wmo_cull.comp.spv")) {
        LOG_ERROR("WMO cull: could not load wmo_cull.comp; no buildings are drawn");
        return false;
    }
    VkComputePipelineCreateInfo pipelineCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineCi.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    pipelineCi.layout = pipelineLayout_;
    const VkResult made =
        vkCreateComputePipelines(device, context.getPipelineCache(), 1, &pipelineCi, nullptr, &pipeline_);
    shader.destroy();
    if (made != VK_SUCCESS) {
        LOG_ERROR("WMO cull: could not build its pipeline (", static_cast<int>(made), "); no buildings are drawn");
        return false;
    }
    noLights_ = createBuffer(context.getAllocator(), 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY);
    if (!noLights_.buffer) {
        LOG_ERROR("WMO cull: no memory for its empty light list; no buildings are drawn");
        return false;
    }
    for (Slot& slot : slots_) {
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        if (context.allocateDescriptorSets(ai, &slot.set) != VK_SUCCESS) {
            LOG_ERROR("WMO cull: could not allocate its sets; no buildings are drawn");
            return false;
        }
    }
    return true;
}

void WmoDrawPass::destroy() {
    if (!context_) return;
    DeferredResources retired(*context_);
    for (Slot& slot : slots_) {
        for (AllocatedBuffer* buffer : {&slot.frame, &slot.bucketStarts, &slot.portalCulled, &slot.commands,
                                        &slot.colourRecords, &slot.casterRecords, &slot.groupLights, &slot.stats})
            if (buffer->buffer) retired.buffer(*buffer);
        slot = Slot{};
    }
    if (noLights_.buffer) retired.buffer(noLights_);
    retired.destroy(pipeline_);
    retired.destroy(pipelineLayout_);
    retired.destroy(pool_);
    retired.destroy(setLayout_);
    instances_.destroy();
    groups_.destroy();
    ranges_.destroy();
    context_ = nullptr;
}

bool WmoDrawPass::ensure(Slot& slot, const WmoDrawFrame& frame) {
    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bool replaced = false;
    const auto grow = [&](AllocatedBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                          VmaMemoryUsage memory, const char* what) {
        return growCullBuffer(*context_, buffer, bytes, usage, memory, "WMORenderer", what, replaced);
    };
    const VkDeviceSize commands =
        VkDeviceSize(2 * frame.colourPlaces + kWmoShadowMaps * frame.casterPlaces) * sizeof(VkDrawIndexedIndirectCommand);
    const bool grown =
        grow(slot.frame, sizeof(WmoCullFrameGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
             "frame view") &&
        grow(slot.bucketStarts, frame.bucketStarts.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VMA_MEMORY_USAGE_CPU_TO_GPU, "bucket starts") &&
        grow(slot.portalCulled, ((frame.groupRowCount + 31) / 32) * sizeof(uint32_t),
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "portal verdict") &&
        grow(slot.commands, commands, kStorage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
             "draw commands") &&
        grow(slot.colourRecords, VkDeviceSize(frame.colourPlaces) * sizeof(glm::uvec2), kStorage,
             VMA_MEMORY_USAGE_GPU_ONLY, "colour records") &&
        grow(slot.casterRecords, VkDeviceSize(frame.casterPlaces) * sizeof(glm::uvec4), kStorage,
             VMA_MEMORY_USAGE_GPU_ONLY, "caster records") &&
        grow(slot.groupLights, VkDeviceSize(frame.groupRowCount) * sizeof(glm::uvec4), kStorage,
             VMA_MEMORY_USAGE_GPU_ONLY, "group lights") &&
        grow(slot.stats, sizeof(WmoCullStatsGPU), kStorage, VMA_MEMORY_USAGE_GPU_TO_CPU, "statistics");
    if (replaced) ++slot.version;
    return grown;
}

void WmoDrawPass::writeSet(Slot& slot) {
    const VkDescriptorBufferInfo infos[kCullBindings] = {
        {instances_.buffer(), 0, instances_.bytes()},
        {groups_.buffer(), 0, groups_.bytes()},
        {ranges_.buffer(), 0, ranges_.bytes()},
        {slot.frame.buffer, 0, sizeof(WmoCullFrameGPU)},
        {slot.bucketStarts.buffer, 0, VK_WHOLE_SIZE},
        {slot.portalCulled.buffer, 0, VK_WHOLE_SIZE},
        {slot.commands.buffer, 0, VK_WHOLE_SIZE},
        {slot.colourRecords.buffer, 0, VK_WHOLE_SIZE},
        {slot.casterRecords.buffer, 0, VK_WHOLE_SIZE},
        {slot.groupLights.buffer, 0, VK_WHOLE_SIZE},
        {slot.stats.buffer, 0, VK_WHOLE_SIZE},
        {slot.lights, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[kCullBindings] = {};
    for (uint32_t b = 0; b < kCullBindings; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = slot.set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = b == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(context_->getDevice(), kCullBindings, writes, 0, nullptr);
    slot.setVersion = slot.version;
}


uint32_t WmoDrawPass::drawVersion(uint32_t slot) const {
    return slots_[slot].version;
}


bool WmoDrawPass::prepare(VkCommandBuffer cmd, uint32_t slotIndex, const WmoDrawFrame& frame, ::VkBuffer lights,
                          uint32_t lightCount) {
    if (slotIndex >= 2) return false;
    Slot& slot = slots_[slotIndex];
    // What this slot's last frame counted: beginFrame waited for it.
    if (std::exchange(slot.counting, false)) readCullCounts(*context_, slot.stats, &counted_, sizeof(counted_));
    slot.ready = false;
    slot.shadowMaps = 0;
    if (!pipeline_) return false;
    if (!lights) {
        lights = noLights_.buffer;
        lightCount = 0;
    }

    // The rows the frame carries, before anything reads the tables.
    if (!instances_.update(cmd, slotIndex, frame.instanceRowCount, frame.instanceSlots, frame.instanceRows.data()) ||
        !groups_.update(cmd, slotIndex, frame.groupRowCount, frame.groupSlots, frame.groupRows.data()) ||
        !ranges_.update(cmd, slotIndex, frame.rangeRowCount, frame.rangeSlots, frame.rangeRows.data())) {
        return false;
    }
    if (frame.groupRowCount == 0 || frame.colourPlaces + frame.casterPlaces == 0) return false;
    if (!ensure(slot, frame)) return false;

    // The draws' sets and the cull's name the tables too.
    const uint32_t tables = instances_.version() * 2654435761u ^ groups_.version() * 40503u ^ ranges_.version();
    if (slot.tablesVersion != tables || slot.lights != lights) {
        slot.tablesVersion = tables;
        slot.lights = lights;
        ++slot.version;
    }
    if (slot.setVersion != slot.version) writeSet(slot);

    WmoCullFrameGPU view = frame.cull;
    view.counts.y = lightCount;
    std::memcpy(slot.frame.info.pMappedData, &view, sizeof(view));
    std::memcpy(slot.bucketStarts.info.pMappedData, frame.bucketStarts.data(),
                frame.bucketStarts.size() * sizeof(uint32_t));
    if (!frame.portalCulled.empty())
        std::memcpy(slot.portalCulled.info.pMappedData, frame.portalCulled.data(),
                    frame.portalCulled.size() * sizeof(uint32_t));
    for (AllocatedBuffer* buffer : {&slot.frame, &slot.bucketStarts, &slot.portalCulled})
        vmaFlushAllocation(context_->getAllocator(), buffer->allocation, 0, VK_WHOLE_SIZE);
    slot.colourPlaces = frame.colourPlaces;
    slot.casterPlaces = frame.casterPlaces;
    slot.groupRows = frame.groupRowCount;

    clearCullTargets(cmd, slot.commands.buffer, slot.stats.buffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &slot.set, 0, nullptr);
    const CullPush push{};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (frame.groupRowCount + 63) / 64, 1, 1);
    finishColourCull(cmd);
    slot.ready = true;
    slot.counting = true;
    return true;
}

std::optional<uint32_t> WmoDrawPass::cullShadowMap(VkCommandBuffer cmd, uint32_t slotIndex, const glm::vec3& centre,
                                                   float reach) {
    if (slotIndex >= 2 || !slots_[slotIndex].ready) return std::nullopt;
    Slot& slot = slots_[slotIndex];
    if (slot.casterPlaces == 0) return std::nullopt;
    if (slot.shadowMaps >= kWmoShadowMaps) {
        static core::LogBudget budget(4, "WMO shadow maps");
        if (budget.take())
            LOG_WARNING("WMO cull: a frame asked for more than ", kWmoShadowMaps,
                        " shadow maps - buildings cast nothing into the rest");
        return std::nullopt;
    }
    const uint32_t map = slot.shadowMaps++;
    beginShadowCull(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &slot.set, 0, nullptr);
    const CullPush push{.depth = glm::vec4(centre, reach), .mode = glm::uvec4(1u, map, 0u, 0u)};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (slot.groupRows + 63) / 64, 1, 1);
    finishShadowCull(cmd);
    return map;
}

} // namespace rendering
} // namespace wowee
