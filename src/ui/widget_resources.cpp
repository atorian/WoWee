#include "ui/widget_renderer.hpp"
#include "ui/widget_tree.hpp"
#include "ui/ui_texture_load.hpp"
#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include <algorithm>
#include <chrono>

namespace wowee::ui {
namespace {
// Descriptor pages grow with the shared image cache instead of consuming the
// fixed font/backend pool. Each set has the ImGui pipeline's layout, just as
// VkContext's existing UI textures do. Pages retire with outstanding snapshots.
class InterfaceDescriptors {
public:
    explicit InterfaceDescriptors(rendering::VkContext& context) : context_(context) {}
    ~InterfaceDescriptors() {
        if (!layout_) return;
        context_.deferAfterAllFrameFences([device = context_.getDevice(),
                                          layout = layout_, pools = std::move(pools_)] {
            for (auto pool : pools) vkDestroyDescriptorPool(device, pool, nullptr);
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        });
    }
    VkDescriptorSet add(const rendering::VkTexture& image) {
        if (!layout_) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = 1; info.pBindings = &binding;
            if (vkCreateDescriptorSetLayout(context_.getDevice(), &info, nullptr, &layout_) != VK_SUCCESS) {
                LOG_ERROR("Interface descriptor layout creation failed; art remains unavailable");
                return VK_NULL_HANDLE;
            }
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        for (;;) {
            if (pools_.empty() || pageFull_) {
                const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pageSize};
                VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                info.maxSets = pageSize; info.poolSizeCount = 1; info.pPoolSizes = &size;
                VkDescriptorPool pool = VK_NULL_HANDLE;
                if (vkCreateDescriptorPool(context_.getDevice(), &info, nullptr, &pool) != VK_SUCCESS) {
                    LOG_ERROR("Interface descriptor page allocation failed; art remains unavailable");
                    return VK_NULL_HANDLE;
                }
                pools_.push_back(pool);
                pageFull_ = false;
                pageUsed_ = 0;
            }
            VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = pools_.back(); info.descriptorSetCount = 1; info.pSetLayouts = &layout_;
            const VkResult result = context_.allocateDescriptorSets(info, &set);
            if (result != VK_SUCCESS) {
                LOG_ERROR("Interface descriptor allocation failed: ", result, "; art remains unavailable");
                return VK_NULL_HANDLE;
            }
            pageFull_ = ++pageUsed_ == pageSize;
            break;
        }
        VkDescriptorImageInfo imageInfo{image.getSampler(), image.getImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(context_.getDevice(), 1, &write, 0, nullptr);
        return set;
    }
private:
    rendering::VkContext& context_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools_;
    static constexpr uint32_t pageSize = 512;
    uint32_t pageUsed_ = 0;
    bool pageFull_ = false;
};
}
WidgetRenderer::~WidgetRenderer() { resetResources(); }
void WidgetRenderer::initialize(pipeline::AssetManager* assets, rendering::VkContext* context) {
    resetResources();
    assets_ = assets;
    vkCtx_ = context;
    if (!assets || !context) return;
    // Asset generations must not find the previous realm/expansion's image
    // under the same virtual path in the process-wide GPU cache.
    static std::atomic<uint64_t> generation{0};
    const std::string prefix = "interface-generation/" + std::to_string(++generation) + "/";
    auto descriptors = std::make_shared<InterfaceDescriptors>(*context);
    textureCache_ = std::make_unique<UiTextureCache>(
        [this, prefix](const std::string& path, bool add) -> PreparedUiTexture {
            std::string resolved;
            const auto bytes = readTextureFile(path, resolved);
            if (bytes.empty()) return {.failure = "BLP file not found or empty"};
            auto image = pipeline::BLPLoader::load(bytes);
            if (!image.isValid()) return {.failure = "BLP decoding failed"};
            if (add) for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
                const auto luminance = std::max({image.data[i], image.data[i+1], image.data[i+2]});
                image.data[i+3] = static_cast<uint8_t>((image.data[i+3] * luminance) / 255);
            }
            auto& cache = vkCtx_->textures();
            const rendering::TextureKey key{prefix + path, rendering::TextureForm::Interface,
                                            static_cast<uint8_t>(add)};
            if (auto existing = cache.find(key)) return {std::move(existing), image.width, image.height, {}};
            if (!cache.reserve(static_cast<uint64_t>(image.width) * image.height * 4))
                return {.failure = "no room in the shared GPU texture budget"};
            auto texture = std::make_shared<rendering::VkTexture>();
            auto completion = std::make_shared<std::atomic<int>>(0);
            PreparedUiTexture result;
            result.pendingImage = texture;
            result.key = key;
            result.uploaded = completion;
            vkCtx_->beginUploadBatch();
            try {
                const bool uploaded = texture->upload(*vkCtx_, image.data.data(), image.width, image.height,
                                                      VK_FORMAT_R8G8B8A8_UNORM, false);
                const bool sampled = uploaded && texture->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR,
                    VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
                if (sampled) { result.width = image.width; result.height = image.height; }
                else result.failure = uploaded ? "sampler creation failed" : "GPU image upload failed";
            } catch (const std::exception& error) {
                result.failure = error.what();
            }
            // Close even after a failed preparation; keep any recorded image
            // alive in the result until teardown has joined its GPU work.
            vkCtx_->endUploadBatch(completion);
            return result;
        },
        [context, descriptors](PreparedUiTexture& ready) {
            if (!ready.image && ready.pendingImage) {
                const auto cost = ready.pendingImage->allocatedBytes();
                ready.image = context->textures().insert(ready.key, std::move(ready.pendingImage), cost);
            }
            return ready.image ? descriptors->add(*ready.image.get()) : VK_NULL_HANDLE;
        },
        [](VkDescriptorSet& descriptor) { descriptor = VK_NULL_HANDLE; });
    registerUiTextureCache(assets_, textureCache_.get());
}
void WidgetRenderer::resetResources() {
    if (artDiscovery_.valid()) {
        try { (void)artDiscovery_.get(); }
        catch (const std::exception& e) { LOG_WARNING("Interface discovery failed during teardown: ", e.what()); }
    }
    if (textureCache_) {
        registerUiTextureCache(assets_, nullptr);
        textureCache_->join();
        // Only dataset changes/shutdown join work; ordinary frames never wait.
        if (vkCtx_) vkCtx_->waitAllUploads();
        textureCache_.reset();
    }
    textureSizes_.clear();
    discoveryStarted_ = false;
    preloadReported_ = false;
    layoutTextureCount_ = 0;
}
void WidgetRenderer::preloadInterface(const std::string& directory) {
    if (discoveryStarted_ || !textureCache_) return;
    discoveryStarted_ = true;
    artDiscovery_ = core::ThreadPool::ioWorkers().submit("interface art discovery", [assets = assets_, directory] {
        std::vector<std::string> paths;
        for (const auto& [path, _] : assets->getManifest().getEntries())
            if (path.starts_with("interface\\") && path.ends_with(".blp")) paths.push_back(path);
        std::sort(paths.begin(), paths.end());
        return discoverInterfaceArt(directory, paths);
    });
    LOG_INFO("Interface art prefetch started at character selection: ", directory);
}
void WidgetRenderer::prefetch(const std::string& path, bool add, bool priority) {
    if (textureCache_) textureCache_->request(path, add, priority);
}
void WidgetRenderer::pumpResources() {
    using namespace std::chrono_literals;
    if (!textureCache_) return;
    const auto start = std::chrono::steady_clock::now();
    if (artDiscovery_.valid() && artDiscovery_.wait_for(0s) == std::future_status::ready) {
        try {
            auto art = artDiscovery_.get();
            for (const auto& ref : art) prefetch(ref.path, ref.additive);
            LOG_INFO("Interface prefetch queued ", art.size(), " window textures");
        } catch (const std::exception& e) {
            LOG_WARNING("Interface prefetch discovery failed: ", e.what(), "; art is queued on demand");
        }
    }
    const auto discovered = std::chrono::steady_clock::now();
    vkCtx_->pollUploadBatches();
    const auto retired = std::chrono::steady_clock::now();
    textureCache_->pump();
    const auto published = std::chrono::steady_clock::now();
    if (published - start > std::chrono::milliseconds(10)) {
        const auto ms = [](auto span) { return std::chrono::duration<double, std::milli>(span).count(); };
        LOG_WARNING("SLOW interface prefetch: discover=", ms(discovered - start),
                    " retire=", ms(retired - discovered), " publish=", ms(published - retired), "ms");
    }
    if (discoveryStarted_ && !artDiscovery_.valid() && !textureCache_->pending() && !preloadReported_) {
        preloadReported_ = true;
        LOG_INFO("Interface prefetch complete: ", textureCache_->ready(), " ready, ",
                 textureCache_->failed(), " unavailable");
    }
}
void WidgetRenderer::prefetchWidgets(const WidgetTree& tree) {
    // A bounded metadata walk, independent of upload throughput. Newly created
    // hidden addon frames join the same queue without opening their window.
    if (prefetchWidgetCursor_ > tree.size()) prefetchWidgetCursor_ = 1;
    const uint32_t end = std::min<uint32_t>(tree.size() + 1, prefetchWidgetCursor_ + 256);
    for (; prefetchWidgetCursor_ < end; ++prefetchWidgetCursor_)
        if (const auto* w = tree.get(prefetchWidgetCursor_)) {
        if (!w->solidColor && !w->externalTexture) prefetch(w->texturePath, w->blendAdd);
        prefetch(w->bgFile); prefetch(w->edgeFile);
        prefetch(w->barTexture); prefetch(w->thumbTexture);
    }
}
const VkDescriptorSet* WidgetRenderer::cachedTexture(const std::string& path, bool add) const {
    return textureCache_ ? textureCache_->resident(path, add) : nullptr;
}
VkDescriptorSet WidgetRenderer::resident(const std::string& path, bool add) const {
    if (path.empty()) return VK_NULL_HANDLE;
    const auto* result = cachedTexture(path, add);
    return result ? *result : VK_NULL_HANDLE;
}
VkDescriptorSet WidgetRenderer::texture(const std::string& path, bool add) {
    if (textureCache_) textureCache_->request(path, add, true);
    return resident(path, add);
}
}
