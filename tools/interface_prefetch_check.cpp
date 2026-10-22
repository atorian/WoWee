// Native cold-resource test: same workers, cache, Vulkan uploads and widgets
// as the client, without a realm or a logged-in character.
#include "core/window.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/vk_context.hpp"
#include "ui/widget_renderer.hpp"
#include "ui/widget_tree.hpp"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    wowee::core::Window window({.title = "Interface prefetch verification", .width = 640, .height = 480});
    if (!window.initialize()) return 2;
    SDL_HideWindow(window.getSDLWindow());
    auto* context = window.getVkContext();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL3_InitForVulkan(window.getSDLWindow());
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = context->getInstance();
    info.PhysicalDevice = context->getPhysicalDevice();
    info.Device = context->getDevice();
    info.QueueFamily = context->getGraphicsQueueFamily();
    info.Queue = context->getGraphicsQueue();
    info.DescriptorPool = context->getImGuiDescriptorPool();
    info.MinImageCount = 2;
    info.ImageCount = context->getSwapchainImageCount();
    info.PipelineInfoMain.RenderPass = context->getOverlayRenderPass();
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info)) return 2;
    const auto shutdownUi = [&] {
        context->waitIdle();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    };
    wowee::pipeline::AssetManager assets;
    if (!assets.initialize(argv[1])) return 2;
    wowee::ui::WidgetRenderer renderer;
    renderer.initialize(&assets, window.getVkContext());
    renderer.preloadInterface(std::string(argv[1]) + "/interface");
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    double maxPump = 0;
    do {
        const auto before = Clock::now();
        renderer.pumpResources();
        maxPump = std::max(maxPump, std::chrono::duration<double, std::milli>(Clock::now() - before).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (Clock::now() - start > std::chrono::seconds(120)) {
            std::fprintf(stderr, "Interface prefetch did not complete\n");
            renderer.resetResources(); shutdownUi(); return 1;
        }
    } while (renderer.resourcesPending());
    const auto resident = renderer.textureCount();
    // These panels have never been constructed, shown or drawn during preload.
    wowee::ui::WidgetTree tree;
    const auto panel = tree.create(wowee::ui::WidgetKind::Frame, tree.root(), "ColdPanel");
    tree.setWidth(panel, 384); tree.setHeight(panel, 512);
    tree.addPoint(panel, {.point = "TOPLEFT", .relativeTo = tree.root(), .relativePoint = "TOPLEFT"});
    for (const char* path : {"Interface\\SpellBook\\UI-SpellbookPanel-TopLeft",
                            "Interface\\PaperDollInfoFrame\\UI-Character-General-TopLeft"}) {
        const auto id = tree.create(wowee::ui::WidgetKind::Texture, panel, "");
        tree.get(id)->texturePath = path;
        tree.setWidth(id, 256); tree.setHeight(id, 256);
        tree.setAllPoints(id, panel);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const auto open = Clock::now();
    renderer.render(tree, 640, 480);
    const double openMs = std::chrono::duration<double, std::milli>(Clock::now() - open).count();
    ImGui::Render();
    const bool drewImages = ImGui::GetDrawData()->TotalVtxCount > 0;
    std::printf("DRAW vertices=%d visibleWidgets=%zu\n", ImGui::GetDrawData()->TotalVtxCount, tree.drawOrder().size());
    uint32_t imageIndex = 0;
    if (const auto command = context->beginFrame(imageIndex)) {
        const VkClearValue clear{.color = {{0, 0, 0, 1}}};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = context->getOverlayClearRenderPass();
        pass.framebuffer = context->getOverlayFramebuffers()[imageIndex];
        pass.renderArea.extent = context->getSwapchainExtent();
        pass.clearValueCount = 1; pass.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
        vkCmdEndRenderPass(command);
        context->endFrame(command, imageIndex);
    }
    const bool loadedBeforeOpen = !renderer.resourcesPending() && renderer.textureCount() == resident;
    std::printf("PREFETCH ready=%zu unavailable=%zu seconds=%.3f maxPumpMs=%.3f firstOpenMs=%.3f noNewLoads=%d\n",
        resident, renderer.unavailableResources(), std::chrono::duration<double>(Clock::now() - start).count(),
        maxPump, openMs, loadedBeforeOpen);
    // Character entry across several characters can exceed the old 2048-set
    // backend pool. Exercise growth with real icons after shared art is ready.
    size_t icons = 0;
    for (const auto& [path, entry] : assets.getManifest().getEntries()) {
        if (path.starts_with("interface\\icons\\") && path.ends_with(".blp")) {
            renderer.prefetch(path, false, true);
            if (++icons == 600) break;
        }
    }
    const auto iconsStart = Clock::now();
    while (renderer.resourcesPending() && Clock::now() - iconsStart < std::chrono::seconds(60)) {
        renderer.pumpResources();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool iconsReady = renderer.textureCount() == resident + icons && !renderer.resourcesPending();
    std::printf("CHARACTER_ICONS requested=%zu ready=%zu total=%zu noFailures=%d\n", icons,
        renderer.textureCount() - resident, renderer.textureCount(), renderer.unavailableResources() == 0);
    const auto bytes = context->textures().stats().form(wowee::rendering::TextureForm::Interface).bytes;
    std::printf("INTERFACE_GPU_MIB %.2f\n", static_cast<double>(bytes) / (1024.0 * 1024.0));
    renderer.resetResources();
    window.getVkContext()->waitIdle();
    shutdownUi();
    return resident > 100 && loadedBeforeOpen && iconsReady && drewImages ? 0 : 1;
}
