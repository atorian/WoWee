#include "rendering/imgui_texture.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "rendering/vk_context.hpp"
#include "rendering/interface_frame.hpp"
#include "core/logger.hpp"
#include <unordered_map>
#include <stdexcept>
#include <utility>

namespace wowee {
namespace rendering {

VkDescriptorSet addImGuiTexture(VkSampler sampler, VkImageView view, VkImageLayout layout) {
    auto* ctx = VkContext::globalInstance();
    if (!ctx) {
        LOG_ERROR("Interface texture registration has no Vulkan context");
        return VK_NULL_HANDLE;
    }
    auto lock = ctx->lockDescriptorPool(ctx->getImGuiDescriptorPool());
    return ImGui_ImplVulkan_AddTexture(sampler, view, layout);
}

void removeImGuiTexture(VkDescriptorSet& descriptorSet) {
    if (descriptorSet == VK_NULL_HANDLE) return;
    if (auto* ctx = VkContext::globalInstance()) {
        const auto set = descriptorSet;
        const auto pool = ctx->getImGuiDescriptorPool();
        ctx->deferAfterAllFrameFences([ctx, pool, set] { ctx->freeDescriptorSets(pool, 1, &set); });
    }
    descriptorSet = VK_NULL_HANDLE;
}

void ImGuiTexture::destroy(VkDevice device, VmaAllocator allocator) {
    removeImGuiTexture(descriptorSet);
    if (texture) {
        if (auto* ctx = VkContext::globalInstance()) {
            std::shared_ptr<VkTexture> image = std::move(texture);
            ctx->deferAfterAllFrameFences([image, device, allocator] { image->destroy(device, allocator); });
        } else {
            texture->destroy(device, allocator);
            texture.reset();
        }
    }
}

ImGuiTexture makeImGuiTexture(VkContext& ctx, const pipeline::BLPImage& image) {
    if (!image.isValid()) return {};

    VkDevice device = ctx.getDevice();
    auto texture = std::make_unique<VkTexture>();
    if (!texture->upload(ctx, image.data.data(), image.width, image.height,
                         VK_FORMAT_R8G8B8A8_UNORM, false)) {
        return {};
    }
    if (!texture->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f)) {
        texture->destroy(device, ctx.getAllocator());
        return {};
    }

    VkDescriptorSet descriptorSet = addImGuiTexture(
        texture->getSampler(), texture->getImageView(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!descriptorSet) {
        texture->destroy(device, ctx.getAllocator());
        return {};
    }

    return ImGuiTexture{.texture = std::move(texture), .descriptorSet = descriptorSet};
}

ImGuiTexture loadImGuiTexture(pipeline::AssetManager& assets, VkContext& ctx,
                              const std::string& path) {
    return makeImGuiTexture(ctx, assets.loadTexture(path));
}

namespace {
struct FontPage {
    VkContext* ctx;
    ImGuiTexture image;
    ~FontPage() { image.destroy(ctx->getDevice(), ctx->getAllocator()); }
};
// Only the interface thread accesses this registry. Frames hold their own
// shared references, independent of the mutable atlas/context registry.
thread_local std::unordered_map<ImGuiContext*, std::unordered_map<ImTextureData*, std::shared_ptr<FontPage>>> fontPages;
}

void releaseImGuiFrameTextures() { fontPages.erase(ImGui::GetCurrentContext()); }

std::shared_ptr<const InterfaceFrame> extractImGuiFrame(VkContext& ctx) {
    const ImDrawData* data = ImGui::GetDrawData();
    if (!data) return {};
    auto frame = std::make_shared<InterfaceFrame>();
    frame->resources = ctx.holdSnapshotResources();
    auto& pages = fontPages[ImGui::GetCurrentContext()];
    if (data->Textures) for (ImTextureData* tex : *data->Textures) {
        if (tex->Status == ImTextureStatus_WantDestroy) {
            pages.erase(tex);
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        } else if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
            pipeline::BLPImage image;
            image.width = tex->Width;
            image.height = tex->Height;
            image.data.resize(static_cast<size_t>(tex->Width) * tex->Height * 4);
            for (size_t p = 0; p < image.data.size() / 4; ++p) {
                if (tex->Format == ImTextureFormat_RGBA32) {
                    std::copy_n(tex->Pixels + p * 4, 4, image.data.data() + p * 4);
                } else {
                    std::fill_n(image.data.data() + p * 4, 3, uint8_t(255));
                    image.data[p * 4 + 3] = tex->Pixels[p];
                }
            }
            auto page = std::make_shared<FontPage>();
            page->ctx = &ctx;
            page->image = makeImGuiTexture(ctx, image);
            if (!page->image || !page->image.descriptorSet) {
                LOG_ERROR("Font atlas upload failed for page ", tex->UniqueID,
                          " (", tex->Width, "x", tex->Height, "); interface frame cannot be drawn");
                throw std::runtime_error("Font atlas upload failed");
            }
            tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(page->image.descriptorSet)));
            tex->SetStatus(ImTextureStatus_OK);
            pages[tex] = std::move(page);
        }
    }
    for (const auto& [tex, page] : pages) { (void)tex; frame->textures.push_back(page); }
    frame->displayPos = {data->DisplayPos.x, data->DisplayPos.y};
    frame->displaySize = {data->DisplaySize.x, data->DisplaySize.y};
    frame->framebufferScale = {data->FramebufferScale.x, data->FramebufferScale.y};
    frame->vertices.reserve(data->TotalVtxCount);
    frame->indices.reserve(data->TotalIdxCount);
    for (const ImDrawList* list : data->CmdLists) {
        const auto baseVertex = static_cast<int32_t>(frame->vertices.size());
        const auto baseIndex = static_cast<uint32_t>(frame->indices.size());
        for (const ImDrawVert& v : list->VtxBuffer)
            frame->vertices.push_back({{v.pos.x, v.pos.y}, {v.uv.x, v.uv.y}, v.col});
        for (ImDrawIdx i : list->IdxBuffer) frame->indices.push_back(i);
        for (const ImDrawCmd& draw : list->CmdBuffer) {
            if (draw.UserCallback == ImDrawCallback_ResetRenderState) continue;
            if (draw.UserCallback) {
                LOG_ERROR("ImGui draw callback must become an owned render request before frame extraction");
                throw std::logic_error("Unowned ImGui draw callback");
            }
            if (!draw.ElemCount) continue;
            const auto set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(draw.GetTexID()));
            if (!set) {
                LOG_ERROR("Interface draw has no prepared texture; frame cannot be drawn");
                throw std::runtime_error("Unprepared interface texture");
            }
            frame->draws.push_back({{draw.ClipRect.x, draw.ClipRect.y, draw.ClipRect.z, draw.ClipRect.w},
                                    set, draw.ElemCount, baseIndex + draw.IdxOffset,
                                    baseVertex + static_cast<int32_t>(draw.VtxOffset)});
        }
    }
    return frame;
}

}  // namespace rendering
}  // namespace wowee
