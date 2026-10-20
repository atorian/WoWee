#pragma once

#include "rendering/vk_shader.hpp"
#include <array>

// GPU regression for the surface consumer, separate from rt_probe's tracing
// checks. A foreground mask is the silhouette in the previous frame. Queries
// cover its shifted edge, uncovered background, a sloping receiver, and places
// where no matching surface exists. Uses the actual rt_lighting.glsli lookup.
inline int checkRtLookup(wowee::rendering::VkContext& ctx, bool benchmark = false,
                         const char* shaderPath = "assets/shaders/rt_lookup_probe.comp.spv",
                         const char* workload = "mixed") {
    using namespace wowee::rendering;
    constexpr uint32_t size = 64;
    struct Sample { glm::vec4 point, settings, normal{0.0f}; };
    struct Result { glm::vec4 ambientWeight, sunAo; };
    std::vector<Sample> samples{
        Sample{{39, 30, 10, 0}, {0, 0, 3, 0}},   // shifted animated edge
        Sample{{35, 30, 20.45f, 0}, {.15f, 0, 3, 0}}, // newly revealed slope
        Sample{{32, 30, 20, 0}, {.15f, 0, 3, 0}}, // wider uncovered strip
        Sample{{50, 30, 22.7f, 0}, {.15f, 0, 3, 0}}, // settled background
        Sample{{39, 30, 50, 0}, {0, 0, 3, 0}},   // unrelated depth: reject
        Sample{{39, 30, 10, 0}, {0, 0, 0, 0}},   // RT off: flat ambient
        Sample{{39, 30, 10, 0}, {0, 0, 2, 0}},   // AO-only mode
        Sample{{39, 30, 10, 0}, {0, 0, 1, 0}},   // sun-only mode
        Sample{{-10, 30, 10, 0}, {0, 0, 3, 0}},  // off-screen taps stay outside
        Sample{{30, 30, 10, 0}, {0, 0, 3, 0}, {1, 0, 0, 0}}, // same distance, wrong plane
        Sample{{30, 30, 10, 0}, {0, 0, 3, 0}, {0, 0, 1, 0}}, // matching lamp lid
        Sample{{30, 30, 10, 0}, {0, 0, 3, 0}, {0, 0, -1, 0}}, // opposite side
        Sample{{8.25f, 59.75f, 30, 0}, {0, 0, 3, 0}, {0, 0, 1, 0}}, // fractional GI gradient
        Sample{{12.75f, 59.25f, 30, 0}, {0, 0, 2, 0}, {0, 0, 1, 0}}, // fractional AO gradient
    };
    samples.push_back(Sample{{36.25f, 30, 10, 0}, {0, 0, 3, 0}, {0, 0, 1, 0}});
    samples.push_back(Sample{{36.75f, 30, 20.7125f, 0}, {.15f, 0, 3, 0}, {0, 0, 1, 0}});
    const size_t checkedSamples = samples.size();
    if (benchmark) {
        // A pixel-sized workload, mostly stable receivers, with silhouette
        // and newly revealed queries mixed in. The actual production lookup
        // is dispatched for each input; no copied implementation is timed.
        samples.reserve(1024 * 1024);
        while (samples.size() < 1024 * 1024) {
            const size_t i = samples.size();
            const float x = 4.25f + float(i % 54);
            Sample query{{x, 58.25f, 30, 0}, {0, 0, 3, 0}, {0, 0, 1, 0}};
            if (std::strcmp(workload, "uncovered") == 0 ||
                (std::strcmp(workload, "mixed") == 0 && i % 8 == 0))
                query = samples[(i / 8) % 12];
            samples.push_back(query);
        }
    }
    const VkDevice dev = ctx.getDevice();
    const auto alloc = ctx.getAllocator();
    auto input = createBuffer(alloc, sizeof(Sample) * samples.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto output = createBuffer(alloc, sizeof(Result) * samples.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VMA_MEMORY_USAGE_CPU_ONLY);
    const size_t imageBytes = size * size * sizeof(glm::vec4);
    auto upload = createBuffer(alloc, imageBytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(input.info.pMappedData, samples.data(), sizeof(Sample) * samples.size());
    auto* texels = static_cast<glm::vec4*>(upload.info.pMappedData);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const bool foreground = x >= 26 && x <= 36 && y >= 16 && y <= 48;
            const size_t i = y * size + x;
            texels[i] = foreground ? glm::vec4(.25f, .4f, 0, 10)
                : glm::vec4(.75f, .65f, 0, 20.0f + .15f * (float(x) - 32.0f));
            texels[size * size + i] = foreground ? glm::vec4(.02f, .03f, .04f, 0)
                                                               : glm::vec4(.08f, .1f, .12f, 0);
            if (y >= 55) {
                // A continuous change of incoming light across one flat
                // receiver must interpolate at fractional texel positions.
                texels[i] = glm::vec4(.2f + .01f * x, .1f + .01f * x, 0, 30);
                texels[size * size + i] = glm::vec4(.02f, .03f, .04f, 1) * (.1f + .01f * x);
            }
        }
    }
    vmaFlushAllocation(alloc, input.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(alloc, upload.allocation, 0, VK_WHOLE_SIZE);
    const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto light = createImage(dev, alloc, size, size, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    auto gi = createImage(dev, alloc, size, size, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.maxLod = 0;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS) return 1;
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
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
    if (!shader.loadFromFile(dev, shaderPath)) return 1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    pci.layout = layout;
    const auto pipelineStatus = vkCreateComputePipelines(dev, ctx.getPipelineCache(), 1, &pci, nullptr, &pipeline);
    shader.destroy();
    if (pipelineStatus != VK_SUCCESS) return 1;
    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
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
        // gi.xy are near zero, which decodes to a +Z geometric normal.
        VkDescriptorImageInfo{sampler, gi.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    std::array<VkWriteDescriptorSet, 5> writes{};
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
        vkCmdDispatch(cmd, (uint32_t(samples.size()) + 63) / 64, 1, 1);
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
                           "AO-only mode", "sun-only mode", "outside image",
                           "wrong plane rejected", "matching lamp lid", "opposite side rejected",
                           "fractional GI gradient", "fractional AO gradient",
                           "foreground bilinear footprint", "background bilinear footprint"};
    int failures = 0;
    for (size_t i = 0; i < checkedSamples; ++i) {
        const bool fallback = i == 4 || i == 5 || i == 8 || i == 9 || i == 11;
        glm::vec3 expectedAmbient = fallback || i == 7 ? glm::vec3(.3f, .35f, .4f)
            : i == 6 ? glm::vec3(.3f, .35f, .4f) * .4f
            : (i == 0 || i == 10) ? glm::vec3(.02f, .03f, .04f) : glm::vec3(.08f, .1f, .12f);
        if (i == 12) expectedAmbient = glm::vec3(.02f, .03f, .04f) * (.1f + .01f * samples[i].point.x);
        if (i == 13) expectedAmbient = glm::vec3(.3f, .35f, .4f) * (.1f + .01f * samples[i].point.x);
        if (i == 14) expectedAmbient = glm::vec3(.02f, .03f, .04f);
        if (i == 15) expectedAmbient = glm::vec3(.08f, .1f, .12f);
        // GI is a signed indirect correction to AO-attenuated ambient,
        // not a replacement that silently drops the Full mode's AO.
        if (!fallback && samples[i].settings.z == 3) {
            const float ao = i == 12 ? .1825f : (i == 0 || i == 10 || i == 14) ? .4f : .65f;
            expectedAmbient += glm::vec3(.3f, .35f, .4f) * ao;
        }
        const float expectedSun = i == 14 ? .25f : i == 15 ? .75f : i >= 12 ? .2f + .01f * samples[i].point.x
            : (i == 0 || i >= 6 ? .25f : .75f);
        const bool ok = glm::length(glm::vec3(results[i].ambientWeight) - expectedAmbient) < (i >= 12 ? 1e-5f : .002f) &&
            std::abs(results[i].ambientWeight.w - (fallback ? 0.0f : 1.0f)) < .002f &&
            (fallback || std::abs(results[i].sunAo.x - expectedSun) < (i >= 12 ? 1e-5f : .002f));
        std::printf("lookup %-30s weight=%.3f ambient=%.3f %.3f %.3f %s\n", names[i],
                    results[i].ambientWeight.w, results[i].ambientWeight.x, results[i].ambientWeight.y,
                    results[i].ambientWeight.z, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
    if (benchmark) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(ctx.getPhysicalDevice(), &properties);
        VkQueryPool poolTimes = VK_NULL_HANDLE;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        if (vkCreateQueryPool(dev, &qci, nullptr, &poolTimes) != VK_SUCCESS) {
            std::fprintf(stderr, "lookup benchmark: failed to create timestamp queries\n");
            ++failures;
        } else {
            std::array<double, 9> milliseconds{};
            for (size_t run = 0; run < milliseconds.size(); ++run) {
                ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                    vkCmdResetQueryPool(cmd, poolTimes, 0, 2);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, poolTimes, 0);
                    vkCmdDispatch(cmd, (uint32_t(samples.size()) + 63) / 64, 1, 1);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, poolTimes, 1);
                });
                uint64_t times[2]{};
                if (vkGetQueryPoolResults(dev, poolTimes, 0, 2, sizeof(times), times, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
                    std::fprintf(stderr, "lookup benchmark: failed to read timestamps\n");
                    ++failures;
                    break;
                }
                milliseconds[run] = double(times[1] - times[0]) * properties.limits.timestampPeriod / 1e6;
            }
            std::sort(milliseconds.begin(), milliseconds.end());
            std::printf("lookup benchmark: %zu receivers, %s, median %.3f ms, min %.3f ms, GPU %s\n",
                        samples.size(), workload, milliseconds[4], milliseconds[0], properties.deviceName);
            vkDestroyQueryPool(dev, poolTimes, nullptr);
        }
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
