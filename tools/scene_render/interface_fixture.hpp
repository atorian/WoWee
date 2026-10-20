#pragma once
#include "rendering/imgui_texture.hpp"
#include "rendering/interface_frame.hpp"
#include "rendering/vk_context.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <stdexcept>

/// Drives the production interface extraction/recorder without the client.
class InterfaceFixture {
public:
    InterfaceFixture(wowee::rendering::VkContext& ctx, int width, int height)
        : ctx_(ctx), width_(width), height_(height) {
        context_ = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_1;
        info.Instance = ctx.getInstance();
        info.PhysicalDevice = ctx.getPhysicalDevice();
        info.Device = ctx.getDevice();
        info.QueueFamily = ctx.getGraphicsQueueFamily();
        info.Queue = ctx.getGraphicsQueue();
        info.DescriptorPool = ctx.getImGuiDescriptorPool();
        info.MinImageCount = 2;
        info.ImageCount = ctx.getSwapchainImageCount();
        info.PipelineInfoMain.RenderPass = ctx.getOverlayRenderPass();
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Interface fixture backend init failed");
    }
    ~InterfaceFixture() {
        ImGui::SetCurrentContext(context_);
        wowee::rendering::releaseImGuiFrameTextures();
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(context_);
    }
    void build(bool change = false) {
        ImGui::SetCurrentContext(context_);
        if (change) { changed_ = true; ImGui::GetIO().Fonts->AddFontDefault(); }
        auto& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 30.0f;
        ImGui::NewFrame();
        auto* draw = ImGui::GetForegroundDrawList();
        draw->PushClipRect(ImVec2(12, 12), ImVec2(width_ - 12.0f, height_ - 12.0f));
        draw->AddRectFilled(ImVec2(24, 24), ImVec2(190, 90), changed_ ? IM_COL32(0, 220, 0, 255) : IM_COL32(220, 0, 0, 255));
        draw->AddText(ImVec2(30, 40), IM_COL32_WHITE, changed_ ? "Grown font atlas ABC xyz 6789" : "Owned interface frame");
        // More than 16-bit vertices, exercising ImDrawCmd::VtxOffset rather
        // than only tiny draw lists whose offsets happen to be zero.
        for (int i = 0; i < 18000; ++i) {
            const float x = 220.0f + static_cast<float>(i % 60);
            const float y = 24.0f + static_cast<float>(i / 60 % 60);
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + 1, y + 1), IM_COL32(40, 80, 200, 255));
        }
        draw->PopClipRect();
        ImGui::Render();
    }
    std::shared_ptr<const wowee::rendering::InterfaceFrame> extract() {
        ImGui::SetCurrentContext(context_);
        return wowee::rendering::extractImGuiFrame(ctx_);
    }
private:
    wowee::rendering::VkContext& ctx_;
    ImGuiContext* context_ = nullptr;
    int width_, height_;
    bool changed_ = false;
};
