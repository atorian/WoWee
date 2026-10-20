#pragma once
#include "rendering/gpu_allocations.hpp"
#include "rendering/rt_scene.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>

inline int checkRtResidency(wowee::rendering::VkContext& ctx) {
    using namespace wowee::rendering;
    RtScene scene;
    if (!scene.initialize(&ctx)) return 1;
    scene.setActive(true);
    auto geometry = [](size_t triangles) {
        RtScene::MeshSource mesh;
        mesh.positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
        mesh.indices.reserve(triangles * 3);
        for (size_t i = 0; i < triangles; ++i) mesh.indices.insert(mesh.indices.end(), {0, 1, 2});
        mesh.surfaces = {packRtSurface(glm::vec3(0.6f), 1)};
        return mesh;
    };
    auto translated = [](glm::vec3 p) { return glm::translate(glm::mat4(1), p); };
    const auto shared = scene.addMesh(geometry(1));
    const auto first = scene.addInstance(shared, translated({0, 0, 0}));
    scene.addInstance(shared, translated({0, 60, 0}));
    const auto heavy = scene.addMesh(geometry(300000));
    scene.addInstance(heavy, translated({4000, 4000, 0}));
    const auto sunward = scene.addInstance(shared, translated({800, 0, 0}));
    RtRegion region;
    region.distance = 100;
    region.indirectLength = 0;
    region.sunlight = false;
    int failures = 0;
    auto require = [&](bool value, const char* description) {
        std::printf("  %-65s %s\n", description, value ? "ok" : "FAIL");
        if (!value) ++failures;
    };
    auto submit = [&](const RtScene::Changes& packet) {
        ctx.cpuStages().clear();
        scene.applyChanges(packet);
        uint32_t image = 0;
        auto cmd = ctx.beginFrame(image);
        if (!cmd) { ++failures; return; }
        scene.update(cmd);
        transitionImageLayout(cmd, ctx.getSwapchainImages()[image],
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        ctx.endFrame(cmd, image);
        ctx.waitForSubmittedFrames("RT residency probe");
    };
    auto settle = [&] { for (int i = 0; i < 8; ++i) submit(scene.extractChanges(region)); };
    auto memory = [&] {
        const auto count = ctx.gpuMemoryByUse();
        return count ? count->deviceLocal(GpuMemoryUse::RayTracing) + count->deviceLocal(GpuMemoryUse::OtherBuffers) : 0;
    };
    settle();
    require(scene.stats().instances == 2 && scene.stats().meshes == 1,
            "near instances share one mesh; distant meshes are not resident");
    const auto small = memory();
    region.sunlight = true;
    region.sunDirection = {1, 0, 0};
    settle();
    require(scene.stats().instances == 3, "offscreen caster toward the sun is retained beyond receiver radius");
    region.sunlight = false;
    region.distance = 10000;
    settle();
    const auto large = memory();
    require(scene.stats().instances == 4 && scene.stats().meshes == 2,
            "increasing the radius admits and builds the distant mesh");
    require(large > small + (4u << 20), "larger region actually allocates more GPU memory");
    region.distance = 100;
    settle();
    const auto reduced = memory();
    std::printf("  RT/buffer bytes: small=%llu large=%llu reduced=%llu\n",
                static_cast<unsigned long long>(small), static_cast<unsigned long long>(large),
                static_cast<unsigned long long>(reduced));
    require(reduced + (4u << 20) < large, "reducing radius releases BLAS and compacts oversized pools");
    region.distance = 10000;
    (void)scene.extractChanges(region); // dropped snapshot must not lose residency edits
    region.distance = 100;
    settle();
    require(scene.stats().instances == 2, "discarded extraction does not leave distant placements resident");
    const auto held = scene.extractChanges(region);
    scene.setInstanceTransform(first, translated({4000, -4000, 0}));
    submit(held);
    require(scene.stats().instances == 2, "an older extracted frame retains its original placement set");
    settle();
    require(scene.stats().instances == 1, "next extraction removes a placement moved outside the region");
    region.centre = {4000, -4000, 0};
    settle();
    require(scene.stats().instances == 1, "moving the camera admits the placement without reloading its model");
    scene.removeInstance(sunward);
    scene.shutdown();
    return failures;
}
