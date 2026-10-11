#pragma once

#include "rendering/vk_shader.hpp"
#include <array>

// GPU regression for the surface consumer, separate from rt_probe's tracing
// checks. A foreground mask is the silhouette in the previous frame. Queries
// cover its shifted edge, uncovered background, a sloping receiver, and places
// where no matching surface exists. Uses the actual rt_lighting.glsli lookup.
inline int checkRtLookup(wowee::rendering::VkContext& ctx) {
    using namespace wowee::rendering;
    constexpr uint32_t size = 64;
    struct Sample { glm::vec4 point, settings; };
    struct Result { glm::vec4 ambientWeight, sunAo; };
    const std::array samples{
        Sample{{39, 30, 10, 0}, {0, 0, 3, 0}},   // shifted animated edge
        Sample{{35, 30, 20.45f, 0}, {.15f, 0, 3, 0}}, // newly revealed slope
        Sample{{32, 30, 20, 0}, {.15f, 0, 3, 0}}, // wider uncovered strip
        Sample{{50, 30, 22.7f, 0}, {.15f, 0, 3, 0}}, // settled background
        Sample{{39, 30, 50, 0}, {0, 0, 3, 0}},   // unrelated depth: reject
        Sample{{39, 30, 10, 0}, {0, 0, 0, 0}},   // RT off: flat ambient
        Sample{{39, 30, 10, 0}, {0, 0, 2, 0}},   // AO-only mode
        Sample{{39, 30, 10, 0}, {0, 0, 1, 0}},   // sun-only mode
        Sample{{-10, 30, 10, 0}, {0, 0, 3, 0}},  // off-screen taps stay outside
    };
    const VkDevice dev = ctx.getDevice();
    const auto alloc = ctx.getAllocator();
    auto input = createBuffer(alloc, sizeof(samples), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto output = createBuffer(alloc, sizeof(Result) * samples.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VMA_MEMORY_USAGE_CPU_ONLY);
    const size_t imageBytes = size * size * sizeof(glm::vec4);
    auto upload = createBuffer(alloc, imageBytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(input.info.pMappedData, samples.data(), sizeof(samples));
    auto* texels = static_cast<glm::vec4*>(upload.info.pMappedData);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const bool foreground = x >= 26 && x <= 36 && y >= 16 && y <= 48;
            const size_t i = y * size + x;
            texels[i] = foreground ? glm::vec4(.25f, .4f, 0, 10)
                : glm::vec4(.75f, .65f, 0, 20.0f + .15f * (float(x) - 32.0f));
            texels[size * size + i] = foreground ? glm::vec4(.02f, .03f, .04f, 0)
                                                               : glm::vec4(.08f, .1f, .12f, 0);
        }
    }
    vmaFlushAllocation(alloc, input.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(alloc, upload.allocation, 0, VK_WHOLE_SIZE);
    const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto light = createImage(dev, alloc, size, size, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    auto gi = createImage(dev, alloc, size, size, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.maxLod = 0;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS) return 1;
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = uint32_t(bindings.size());
    slci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &setLayout) != VK_SUCCESS) return 1;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &layout) != VK_SUCCESS) return 1;
    wowee::rendering::VkShaderModule shader;
    if (!shader.loadFromFile(dev, "assets/shaders/rt_lookup_probe.comp.spv")) return 1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    pci.layout = layout;
    const auto pipelineStatus = vkCreateComputePipelines(dev, ctx.getPipelineCache(), 1, &pci, nullptr, &pipeline);
    shader.destroy();
    if (pipelineStatus != VK_SUCCESS) return 1;
    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = uint32_t(poolSizes.size());
    dpci.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &pool) != VK_SUCCESS) return 1;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(dev, &dsai, &set) != VK_SUCCESS) return 1;
    const std::array bufferInfos{
        VkDescriptorBufferInfo{input.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{output.buffer, 0, VK_WHOLE_SIZE},
    };
    const std::array imageInfos{
        VkDescriptorImageInfo{sampler, light.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        VkDescriptorImageInfo{sampler, gi.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (size_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = bindings[i].binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        if (i < 2) writes[i].pBufferInfo = &bufferInfos[i];
        else writes[i].pImageInfo = &imageInfos[i - 2];
    }
    vkUpdateDescriptorSets(dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        for (int i = 0; i < 2; ++i) {
            const auto& img = i == 0 ? light : gi;
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            cmdPipelineBarrier2(cmd, dep);
            VkBufferImageCopy copy{};
            copy.bufferOffset = i * imageBytes;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {size, size, 1};
            vkCmdCopyBufferToImage(cmd, upload.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            cmdPipelineBarrier2(cmd, dep);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, uint32_t(samples.size()), 1, 1);
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    });
    vmaInvalidateAllocation(alloc, output.allocation, 0, VK_WHOLE_SIZE);
    const auto* results = static_cast<const Result*>(output.info.pMappedData);
    const std::array names{"moving character edge", "revealed sloping background", "wide revealed strip",
                           "stable sloping background", "unrelated depth rejected", "RT off",
                           "AO-only mode", "sun-only mode", "outside image"};
    int failures = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const bool fallback = i == 4 || i == 5 || i == 8;
        glm::vec3 expectedAmbient = fallback || i == 7 ? glm::vec3(.3f, .35f, .4f)
            : i == 6 ? glm::vec3(.3f, .35f, .4f) * .4f
            : i == 0 ? glm::vec3(.02f, .03f, .04f) : glm::vec3(.08f, .1f, .12f);
        const bool ok = glm::length(glm::vec3(results[i].ambientWeight) - expectedAmbient) < .002f &&
            std::abs(results[i].ambientWeight.w - (fallback ? 0.0f : 1.0f)) < .002f &&
            (fallback || std::abs(results[i].sunAo.x - (i == 0 || i >= 6 ? .25f : .75f)) < .002f);
        std::printf("lookup %-30s weight=%.3f ambient=%.3f %.3f %.3f %s\n", names[i],
                    results[i].ambientWeight.w, results[i].ambientWeight.x, results[i].ambientWeight.y,
                    results[i].ambientWeight.z, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
    destroy(dev, pipeline);
    destroy(dev, layout);
    destroy(dev, pool);
    destroy(dev, setLayout);
    destroy(dev, sampler);
    destroyImage(dev, alloc, light);
    destroyImage(dev, alloc, gi);
    destroyBuffer(alloc, input);
    destroyBuffer(alloc, output);
    destroyBuffer(alloc, upload);
    return failures;
}
