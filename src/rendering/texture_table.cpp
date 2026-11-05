#include "rendering/texture_table.hpp"

#include "foundation/logger.hpp"

#include <algorithm>
#include <array>

namespace wowee {
namespace rendering {

namespace {

constexpr uint32_t kPlainBinding = 0;
constexpr uint32_t kLayeredBinding = 1;
constexpr uint32_t kSamplerBinding = 2;

// What the sets bound beside the table may sample and hold samplers for: the
// per-frame block's shadow maps and the like, and a renderer's own.
constexpr uint32_t kReserved = 64;

} // namespace

TextureTable::Capacity TextureTable::capacityFor(const VkPhysicalDeviceLimits& limits,
                                                 const VkPhysicalDeviceVulkan12Properties& properties12) {
    const auto room = [](uint32_t limit) { return limit > kReserved ? limit - kReserved : 0u; };
    Capacity capacity;
    capacity.layered = std::min({4096u, room(properties12.maxPerStageDescriptorUpdateAfterBindSampledImages) / 2,
                                 room(properties12.maxDescriptorSetUpdateAfterBindSampledImages) / 2});
    const uint32_t images = std::min(room(properties12.maxPerStageDescriptorUpdateAfterBindSampledImages),
                                     room(properties12.maxDescriptorSetUpdateAfterBindSampledImages));
    capacity.plain = std::min({65536u, kMaxImages, images - capacity.layered});
    capacity.samplers = std::min({1024u, kMaxSamplers, room(properties12.maxPerStageDescriptorUpdateAfterBindSamplers),
                                  room(properties12.maxDescriptorSetUpdateAfterBindSamplers),
                                  room(limits.maxSamplerAllocationCount)});
    return capacity;
}

TextureTable::~TextureTable() { shutdown(); }

bool TextureTable::initialize(VkDevice device, Capacity capacity) {
    device_ = device;
    plain_.capacity = capacity.plain;
    layered_.capacity = capacity.layered;
    samplerCapacity_ = capacity.samplers;
    if (!capacity.plain || !capacity.layered || !capacity.samplers) {
        LOG_ERROR("Texture table: the device allows ", capacity.plain, " plain and ", capacity.layered,
                  " layered images and ", capacity.samplers,
                  " samplers bound after binding - nothing can be drawn by texture handle");
        return false;
    }

    const VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {.binding = kPlainBinding, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = capacity.plain, .stageFlags = stages},
        {.binding = kLayeredBinding, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = capacity.layered, .stageFlags = stages},
        {.binding = kSamplerBinding, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
         .descriptorCount = capacity.samplers, .stageFlags = stages},
    }};
    // A place is written while frames that do not use it are in flight, and
    // most places are never written at all.
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const std::array<VkDescriptorBindingFlags, 3> bindingFlags = {flags, flags, flags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    flagsInfo.pBindingFlags = bindingFlags.data();
    VkDescriptorSetLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        LOG_ERROR("Texture table: could not make its set layout (", capacity.plain, " plain, ", capacity.layered,
                  " layered, ", capacity.samplers, " samplers)");
        return false;
    }

    const std::array<VkDescriptorPoolSize, 2> sizes = {{
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = capacity.plain + capacity.layered},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = capacity.samplers},
    }};
    VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR("Texture table: could not make its descriptor pool");
        return false;
    }
    VkDescriptorSetAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(device, &allocInfo, &set_) != VK_SUCCESS) {
        LOG_ERROR("Texture table: could not allocate its set");
        return false;
    }
    LOG_INFO("Texture table: ", capacity.plain, " plain and ", capacity.layered, " layered images, ",
             capacity.samplers, " samplers");
    return true;
}

void TextureTable::shutdown() {
    if (!device_) return;
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

uint32_t TextureTable::samplerPlaceLocked(VkSampler sampler) {
    if (auto it = samplers_.find(sampler); it != samplers_.end()) return it->second;
    const auto place = static_cast<uint32_t>(samplers_.size());
    if (place >= samplerCapacity_) return kMaxSamplers;
    VkDescriptorImageInfo info{.sampler = sampler};
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = kSamplerBinding;
    write.dstArrayElement = place;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    samplers_.emplace(sampler, place);
    return place;
}

TextureHandle TextureTable::add(VkImageView view, VkSampler sampler, Kind kind, VkImageLayout layout) {
    if (!set_ || !view || !sampler) return kNoTextureHandle;
    std::lock_guard<std::mutex> lock(mutex_);
    Places& places = this->places(kind);
    const uint32_t samplerPlace = samplerPlaceLocked(sampler);
    if (samplerPlace >= samplerCapacity_ || (places.free.empty() && places.next >= places.capacity)) {
        if (!places.fullSaid) {
            LOG_ERROR("Texture table full: ", places.used, " of ", places.capacity,
                      kind == Kind::Plain ? " plain" : " layered", " images and ", samplers_.size(), " of ",
                      samplerCapacity_, " samplers in use; a texture past them is drawn with its fallback");
            places.fullSaid = true;
        }
        return kNoTextureHandle;
    }
    places.fullSaid = false;
    uint32_t place = 0;
    if (!places.free.empty()) {
        place = places.free.back();
        places.free.pop_back();
    } else {
        place = places.next++;
    }
    ++places.used;
    VkDescriptorImageInfo info{.imageView = view, .imageLayout = layout};
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = kind == Kind::Plain ? kPlainBinding : kLayeredBinding;
    write.dstArrayElement = place;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return place | (samplerPlace << kImageBits);
}

void TextureTable::remove(TextureHandle handle, Kind kind) {
    if (handle == kNoTextureHandle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Places& places = this->places(kind);
    places.free.push_back(handle & (kMaxImages - 1));
    --places.used;
}

TextureHandle TextureTable::withSampler(TextureHandle handle, VkSampler sampler) {
    if (handle == kNoTextureHandle || !set_ || !sampler) return kNoTextureHandle;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t samplerPlace = samplerPlaceLocked(sampler);
    if (samplerPlace >= samplerCapacity_) {
        if (!samplersFullSaid_) {
            LOG_ERROR("Texture table full: all ", samplerCapacity_,
                      " sampler places in use; a texture drawn through another sampler is drawn with its fallback");
            samplersFullSaid_ = true;
        }
        return kNoTextureHandle;
    }
    return (handle & (kMaxImages - 1)) | (samplerPlace << kImageBits);
}

uint32_t TextureTable::used(Kind kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kind == Kind::Plain ? plain_.used : layered_.used;
}

} // namespace rendering
} // namespace wowee
