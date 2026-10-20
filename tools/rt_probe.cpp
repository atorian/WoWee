// rt_probe: the ray traced lighting pass, run once against a scene whose
// answer is known, on whatever Vulkan device this machine has.
//
// The pass only runs in the world, and the world needs a realm. This stands
// up the real context and the real RtScene / RtLighting on a hidden window,
// puts a box between a wall and the sun, fakes the wall's depth buffer, traces
// one frame and reads the result back:
//
//   - a wall point whose ray to the sun crosses the box must be in shadow,
//   - wall points whose rays miss it must be lit,
//   - every value must be finite.
//
//     cmake -S . -B build -DWOWEE_BUILD_RT_PROBE=ON
//     cmake --build build --target rt_probe
//     cd build/bin && ./rt_probe            (exit status 0 = pass)
//     ./rt_probe --lookup-only [shader.spv] [stable|mixed|uncovered]
//     ./rt_probe --translated --sweep    (AO/GI and mode changes)
//     ./rt_probe --reflections             (explicit mirror consumer)
//     ./rt_probe --translated               (continent-coordinate precision)
//
// WOWEE_RT_FORCE_SOFTWARE=1 selects the compute tracer on hardware that has
// ray queries, so both backends can be checked on one machine.

#include "core/logger.hpp"
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rt_lookup_probe.hpp"

using namespace wowee::rendering;

namespace {

constexpr uint32_t kSize = 1024;       // scene depth, square
constexpr float kWallDistance = 20.0f;

RtScene::MeshSource box(const glm::vec3& lo, const glm::vec3& hi) {
    RtScene::MeshSource m;
    for (int i = 0; i < 8; ++i) {
        m.positions.emplace_back((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
    }
    const uint32_t faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                                  {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    for (const auto& f : faces) {
        for (uint32_t idx : {f[0], f[1], f[2], f[0], f[2], f[3]}) m.indices.push_back(idx);
    }
    m.surfaces.push_back(packRtSurface(glm::vec3(0.6f), 1.0f));
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    const auto hasArgument = [&](const char* name) {
        for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], name) == 0) return true;
        return false;
    };
    // World translation must not change reconstructed rays or lighting.
    const glm::vec3 camera = argc > 1 && std::strcmp(argv[1], "--translated") == 0
        ? glm::vec3(5800.0f, 600.0f, 650.0f) : glm::vec3(0.0f);
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::INFO);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Window* window = SDL_CreateWindow("rt_probe", kSize, kSize,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 2;
    }

    int status = 1;
    {
        VkContext ctx;
        if (!ctx.initialize(window)) {
            std::fprintf(stderr, "VkContext failed\n");
            return 2;
        }
        if (argc > 1 && std::strcmp(argv[1], "--lookup-only") == 0) {
            if (argc > 3 && std::strcmp(argv[3], "stable") != 0 && std::strcmp(argv[3], "mixed") != 0 &&
                std::strcmp(argv[3], "uncovered") != 0) {
                std::fprintf(stderr, "lookup workload must be stable, mixed or uncovered\n");
                return 2;
            }
            const int failures = checkRtLookup(ctx, true,
                argc > 2 ? argv[2] : "assets/shaders/rt_lookup_probe.comp.spv", argc > 3 ? argv[3] : "mixed");
            ctx.flushDeferredCleanup();
            ctx.shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
            return failures == 0 ? 0 : 1;
        }
        RtScene scene;
        RtLighting lighting;
        if (!scene.initialize(&ctx) || !lighting.initialize(&ctx, &scene)) {
            std::fprintf(stderr, "RT init failed\n");
            return 2;
        }
        std::printf("backend: %s\n",
                    scene.backend() == RtScene::Backend::Hardware ? "hardware" : "compute");

        auto reflectedBuilding = box({-8, -22, -100}, {-6, -12, 100});
        reflectedBuilding.surfaces[0] = packRtSurface(glm::vec3(0.8f, 0.1f, 0.05f), 1.0f);

        const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(37.0f), glm::vec3(0, 0, 1)) *
                                   glm::rotate(glm::mat4(1.0f), glm::radians(23.0f), glm::vec3(0, 1, 0));
        const glm::mat4 sceneTransform = glm::translate(glm::mat4(1.0f), camera) * rotation;

        // A 2x4x4 box five yards in front of the wall. The camera looks down
        // +X with Z up, as the client's does.
        const RtScene::MeshId mesh = scene.addMesh(box({14, -2, -2}, {16, 2, 2}));
        scene.addInstance(mesh, sceneTransform);
        // A second instance of it, moved, to exercise the top level.
        scene.addInstance(mesh, sceneTransform * glm::translate(glm::mat4(1.0f), glm::vec3(0, 40, 0)));
        // Include the receiving wall: bad reconstructed origins can hit it.
        const auto wall = scene.addMesh(box({kWallDistance, -100, -100},
                                             {kWallDistance + 0.1f, 100, 100}));
        scene.addInstance(wall, sceneTransform);
        scene.addInstance(scene.addMesh(std::move(reflectedBuilding)), sceneTransform);

        glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.5f, 2000.0f);
        proj[1][1] *= -1.0f;
        const glm::mat4 view = glm::lookAt(camera, camera + glm::vec3(rotation * glm::vec4(1, 0, 0, 0)),
                                             glm::vec3(rotation * glm::vec4(0, 0, 1, 0)));
        const glm::mat4 viewProj = proj * view;
        const glm::vec4 wallClip = viewProj * sceneTransform * glm::vec4(kWallDistance, 0, 0, 1);
        const float wallDepth = wallClip.z / wallClip.w;

        const VkExtent2D sceneExtent{kSize, kSize};
        AllocatedImage depth = createImage(ctx.getDevice(), ctx.getAllocator(), kSize, kSize,
                                           ctx.getDepthFormat(),
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        lighting.setMode(RtLighting::Mode::Full);

        RtLighting::FrameInputs in{};
        in.view = view;
        in.projection = proj;
        in.cameraPos = camera;
        std::printf("camera: %.0f %.0f %.0f\n", camera.x, camera.y, camera.z);
        in.sunDir = glm::normalize(glm::vec3(rotation * glm::vec4(-1.0f, 0.0f, 0.25f, 0.0f)));  // toward the sun
        in.sunColor = glm::vec3(1.0f);
        in.skyColor = glm::vec3(0.3f, 0.35f, 0.4f);
        in.sunUp = true;
        in.traceReflections = hasArgument("--reflections");

        auto depthLayout = [&](VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = depth.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            cmdPipelineBarrier2(cmd, dep);
        };
        bool depthInitialized = false;
        const auto recordFrame = [&]() {
            lighting.prepare(sceneExtent);
            uint32_t presentImage = 0;
            VkCommandBuffer cmd = ctx.beginFrame(presentImage);
            if (cmd == VK_NULL_HANDLE) {
                std::fprintf(stderr, "rt_probe: could not begin a GPU frame\n");
                return uint32_t(MAX_FRAMES_IN_FLIGHT);
            }
            const uint32_t slot = ctx.getCurrentFrame();
            if (!depthInitialized) {
                depthLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkClearDepthStencilValue clear{wallDepth, 0};
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                vkCmdClearDepthStencilImage(cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            &clear, 1, &range);
                depthLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                depthInitialized = true;
            }
            lighting.record(cmd, depth.image, sceneExtent, false, in);
            // Follow the real submission path: it resets GPU timestamp pools,
            // rotates history slots and fences their writes before readback.
            transitionImageLayout(cmd, ctx.getSwapchainImages()[presentImage],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            ctx.endFrame(cmd, presentImage);
            ctx.waitForSubmittedFrames("rt_probe trace");
            return slot;
        };
        const uint32_t recordedSlot = recordFrame();
        if (recordedSlot >= MAX_FRAMES_IN_FLIGHT) return 2;

        const VkExtent2D te = lighting.traceExtent();
        const VkDeviceSize bytes = VkDeviceSize(te.width) * te.height * 8;
        AllocatedBuffer light = createBuffer(ctx.getAllocator(), bytes,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VMA_MEMORY_USAGE_CPU_ONLY);
        AllocatedBuffer gi = createBuffer(ctx.getAllocator(), bytes,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VMA_MEMORY_USAGE_CPU_ONLY);
        AllocatedBuffer reflection = createBuffer(ctx.getAllocator(), bytes,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
        const auto readOutputs = [&](uint32_t slot) {
            ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {te.width, te.height, 1};
                vkCmdCopyImageToBuffer(cmd, lighting.lightImageWrittenBy(slot),
                                       VK_IMAGE_LAYOUT_GENERAL, light.buffer, 1, &region);
                vkCmdCopyImageToBuffer(cmd, lighting.giImageWrittenBy(slot),
                                       VK_IMAGE_LAYOUT_GENERAL, gi.buffer, 1, &region);
                vkCmdCopyImageToBuffer(cmd, lighting.reflectionImageWrittenBy(slot),
                                       VK_IMAGE_LAYOUT_GENERAL, reflection.buffer, 1, &region);
            });

            for (const auto* buffer : {&light, &gi, &reflection})
                vmaInvalidateAllocation(ctx.getAllocator(), buffer->allocation, 0, VK_WHOLE_SIZE);
        };
        readOutputs(recordedSlot);

        const auto* lightTexels = static_cast<const uint16_t*>(light.info.pMappedData);
        const auto* giTexels = static_cast<const uint16_t*>(gi.info.pMappedData);
        auto texel = [&](const uint16_t* base, uint32_t x, uint32_t y, int c) {
            return glm::unpackHalf1x16(base[(size_t(y) * te.width + x) * 4 + c]);
        };
        auto sample = [&](const glm::vec3& world, float& sun, float& dist) {
            const glm::vec4 c = viewProj * sceneTransform * glm::vec4(world, 1.0f);
            const glm::vec2 uv = glm::vec2(c) / c.w * 0.5f + 0.5f;
            const auto x = static_cast<uint32_t>(uv.x * te.width);
            const auto y = static_cast<uint32_t>(uv.y * te.height);
            sun = texel(lightTexels, x, y, 0);
            dist = texel(lightTexels, x, y, 3);
        };

        const auto* reflectionTexels = static_cast<const uint16_t*>(reflection.info.pMappedData);
        int failures = checkRtLookup(ctx);
        auto expect = [&](const char* what, bool ok) {
            std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        const auto rx = te.width * 3 / 4, ry = te.height / 2;
        const glm::vec3 reflected(texel(reflectionTexels, rx, ry, 0),
                                  texel(reflectionTexels, rx, ry, 1),
                                  texel(reflectionTexels, rx, ry, 2));
        std::printf("offscreen building reflection: %.3f %.3f %.3f\n", reflected.r, reflected.g, reflected.b);
        if (in.traceReflections) {
            expect("mirror consumer receives the red building behind the camera",
                   reflected.r > .2f && reflected.r > reflected.g * 4 &&
                   reflected.g > reflected.b && texel(reflectionTexels, rx, ry, 3) > 0);
        } else {
            expect("no mirror consumer means no reflection is generated",
                   glm::length(reflected) < .001f && texel(reflectionTexels, rx, ry, 3) == 0);
        }

        // Rays toward the sun rise a quarter yard per yard: from the wall, the
        // box's front face (x = 16) is four yards back, so a wall point is
        // shadowed for z in about [-3, 1] and |y| < 2.
        float sun = 0, dist = 0;
        sample({kWallDistance, 0.0f, -1.0f}, sun, dist);
        std::printf("wall behind box: sun=%.3f dist=%.2f\n", sun, dist);
        expect("wall point behind the box is shadowed", sun < 0.25f);
        expect("stored distance matches the wall", std::abs(dist - kWallDistance) < 0.5f);
        sample({kWallDistance, 0.0f, 5.0f}, sun, dist);
        std::printf("wall above shadow: sun=%.3f\n", sun);
        expect("wall point above the shadow is lit", sun > 0.75f);
        sample({kWallDistance, 7.0f, 0.0f}, sun, dist);
        std::printf("wall beside shadow: sun=%.3f\n", sun);
        expect("wall point beside the shadow is lit", sun > 0.75f);

        bool finite = true;
        uint32_t shadowed = 0, acne = 0;
        float minSun = 1.0f, minAo = 1.0f;
        for (uint32_t y = 0; y < te.height; ++y) {
            for (uint32_t x = 0; x < te.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    finite = finite && std::isfinite(texel(lightTexels, x, y, c)) &&
                             std::isfinite(texel(giTexels, x, y, c));
                }
                if (texel(lightTexels, x, y, 0) < 0.5f) ++shadowed;
                // Far from the box shadow and image border, this flat wall
                // has neither blockers toward the sun nor AO occluders.
                const float wallY = -(float(x * 2) + 0.5f - kSize * 0.5f) *
                                    (2.0f * kWallDistance / (kSize * proj[0][0]));
                if (x > 8 && x + 8 < te.width && y > 8 && y + 8 < te.height &&
                    std::abs(wallY) > 5.0f && std::abs(wallY) < 10.0f) {
                    const float visibility = texel(lightTexels, x, y, 0);
                    const float ao = texel(lightTexels, x, y, 1);
                    minSun = std::min(minSun, visibility);
                    minAo = std::min(minAo, ao);
                    if (visibility < 0.95f || ao < 0.95f) ++acne;
                }
            }
        }
        std::printf("trace %ux%u, %u shadowed texels\n", te.width, te.height, shadowed);
        std::printf("unoccluded wall: %u acne texels, min sun=%.4f AO=%.4f\n", acne, minSun, minAo);
        expect("unoccluded receiving wall has no self-shadow speckles", acne == 0);
        expect("every value is finite", finite);
        expect("the shadow is a patch, not the frame", shadowed > 20 && shadowed < te.width * te.height / 3);

        if (hasArgument("--sweep")) {
            // A nearby opaque plane covers the receiver's hemisphere. AO
            // attenuates ambient; the one-bounce light returns the plane's
            // known albedo times sky radiance. Switching through lower modes
            // must not reuse the previous mode's lighting history.
            auto nearPlane = box({19.0f, -200, -200}, {19.1f, 200, 200});
            nearPlane.surfaces[0] = packRtSurface(glm::vec3(.2f, .4f, .6f), 1.0f);
            scene.addInstance(scene.addMesh(std::move(nearPlane)), sceneTransform);
            in.sunUp = false;
            in.traceReflections = false;
            for (auto mode : {RtLighting::Mode::ShadowsAO, RtLighting::Mode::Full,
                              RtLighting::Mode::Shadows, RtLighting::Mode::Full}) {
                lighting.setMode(mode);
                const uint32_t slot = recordFrame();
                if (slot >= MAX_FRAMES_IN_FLIGHT) return 2;
                readOutputs(slot);
                glm::dvec3 incoming(0);
                double aoMean = 0;
                size_t count = 0;
                for (uint32_t y = te.height / 4; y < te.height * 3 / 4; ++y) {
                    for (uint32_t x = te.width / 4; x < te.width * 3 / 4; ++x) {
                        const float ao = texel(lightTexels, x, y, 1);
                        aoMean += ao;
                        glm::vec3 value = in.skyColor;
                        if (mode != RtLighting::Mode::Shadows) value *= ao;
                        if (mode == RtLighting::Mode::Full) {
                            value += glm::vec3(texel(giTexels, x, y, 0), texel(giTexels, x, y, 1),
                                               texel(giTexels, x, y, 2));
                        }
                        incoming += glm::dvec3(value);
                        ++count;
                    }
                }
                aoMean /= double(count);
                incoming /= double(count);
                std::printf("mode %d: AO %.4f incoming %.4f %.4f %.4f\n", int(mode), aoMean,
                            incoming.x, incoming.y, incoming.z);
                if (mode == RtLighting::Mode::Shadows) {
                    expect("shadows mode leaves ambient unoccluded", std::abs(aoMean - 1) < .001);
                } else {
                    expect("the nearby plane occludes ambient in AO and Full", aoMean > .1 && aoMean < .8);
                }
                if (mode == RtLighting::Mode::Full) {
                    const glm::dvec3 expected = glm::dvec3(in.skyColor) * glm::dvec3(.2, .4, .6);
                    expect("Full retains AO and one-bounce light without double occlusion",
                           glm::length(incoming - expected) < .01);
                }
            }
        }

        destroyBuffer(ctx.getAllocator(), light);
        destroyBuffer(ctx.getAllocator(), gi);
        destroyBuffer(ctx.getAllocator(), reflection);
        destroyImage(ctx.getDevice(), ctx.getAllocator(), depth);
        ctx.flushDeferredCleanup();
        lighting.shutdown();
        scene.shutdown();
        ctx.shutdown();
        status = failures == 0 ? 0 : 1;
        std::printf("%s\n", status == 0 ? "PASS" : "FAIL");
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
