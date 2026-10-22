#pragma once

// Draws a WidgetTree.
//
// Kept apart from the tree itself so the layout rules stay testable without a
// Vulkan device. This half is the part that needs one.
//
// Textures come from the game's own Interface\ art through the existing asset
// path - read the BLP, upload it, hand ImGui the descriptor set - which is the
// same route the action bar already takes for its backpack button. Nothing new
// is shipped; it is the player's own install being drawn.

#include "ui/ui_texture_cache.hpp"
#include "ui/interface_art.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

struct ImDrawList;   // global, as ImGui declares it
struct ImFont;
struct ImVec2;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class VkContext; class CpuStageTimes; }

namespace ui {

class WidgetTree;
struct Widget;

class WidgetRenderer {
public:
    ~WidgetRenderer();
    void resetResources();
    void preloadInterface(const std::string& directory);
    void prefetch(const std::string& path, bool additive = false, bool priority = false);
    void pumpResources();
    bool resourcesPending() const { return artDiscovery_.valid() || (textureCache_ && textureCache_->pending()); }
    size_t unavailableResources() const { return textureCache_ ? textureCache_->failed() : 0; }
    // Includes hidden widgets and templates materialized by addons.
    void prefetchWidgets(const WidgetTree& tree);
    void initialize(pipeline::AssetManager* assets, rendering::VkContext* vkCtx);
    /// Where draw() records what its parts cost, for the frame profile.
    void setStageTimes(rendering::CpuStageTimes* times) { stageTimes_ = times; }

    /// Lay the tree out for this screen and draw it. Safe to call with no
    /// device or assets - it simply does nothing, which is what the headless
    /// tests want.
    void render(WidgetTree& tree, float screenW, float screenH);

    /// The two halves of render(), for callers that need something to happen
    /// between them.
    ///
    /// Hit testing reads the rects layout() produces, so it has to run early -
    /// before the frame's clicks are resolved, or a frame that moved this frame
    /// is clicked where it used to be. Drawing has the opposite requirement:
    /// the interface sits over the world, and the world's overlays - the
    /// nameplates and the minimap's blips - go into the same ImGui background
    /// list this does, where whatever is added last is on top. Drawn from here
    /// the panels went down first and every nameplate in the world showed
    /// through the bags.
    void layout(WidgetTree& tree, float screenW, float screenH);
    void draw(WidgetTree& tree, float screenW, float screenH);

    /// What is on screen, and what should be but is not - the instrumentation
    /// the FrameXML transition is being carried out with. Draws nothing.
    void reportWidgetDiagnostics(WidgetTree& tree, const std::vector<const Widget*>& order,
                                 float s, float screenW, float screenH);

    /// Ready UI images retained for this dataset under the shared GPU budget.
    [[nodiscard]] size_t textureCount() const { return textureCache_ ? textureCache_->ready() : 0; }

    /// Whether the art at this path can be read and decoded at all, and how big
    /// it is. Public because it answers a question worth asking from outside:
    /// FrameXML naming art the assets do not carry is a blank where an icon
    /// should be, and nothing else reports it - the draw silently substitutes
    /// nothing and carries on.
    bool artResolves(const std::string& path, float& w, float& h) {
        return textureSize(path, w, h);
    }

private:
    /// Request a path, then return its already-ready descriptor. A queued
    /// image and a failed image both draw nothing, but only failure is cached.
    VkDescriptorSet texture(const std::string& path, bool add = false);
    /// Already-uploaded texture for a path, without triggering an upload.
    [[nodiscard]] VkDescriptorSet resident(const std::string& path, bool add = false) const;

    /// A label or a texture, as the tree places it (WidgetTree::setContentSizer).
    ///
    /// A FontString with no <Size> takes the size of its string, as it does in
    /// WoW. Leaving it at zero lays it out to nothing and draws nothing, so the
    /// text is set, correct, and invisible - the player frame's level number
    /// read text="14" in a rect of 0x0. A texture takes the dimensions of its
    /// own image on any axis nothing else decides; FrameXML depends on that -
    /// the friends list's status icon declares one anchor and no size.
    ///
    /// Measured as each is placed rather than in a pass over the whole tree
    /// before layout, which touched all 28,000 widgets to size the few hundred
    /// on screen.
    void sizeContent(Widget& w);
    /// The face a label falls back to, found once a layout rather than per
    /// label; null until the first layout.
    ImFont* sizingFont_ = nullptr;
    /// Held by the sizer the tree calls, so a tree that outlives this
    /// renderer finds it gone rather than calling into freed memory.
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
    void sizeTextureWidget(Widget* w);
    void sizeFontStringWidget(Widget* w, ImFont* font);
    /// Sizes a tooltip to the lines it holds. A tooltip has no size until it
    /// has something to say.
    void sizeTooltipWidget(Widget* w, ImFont* font, WidgetTree& tree);
    /// How big the picture is, without uploading it. No Vulkan context needed:
    /// asking a file its dimensions does not require a GPU, and requiring one
    /// would put this beyond the reach of the headless harness.
    bool textureSize(const std::string& path, float& w, float& h);
    /// The bytes of a texture, with the extension and folder fallbacks applied.
    /// Shared by the upload and the size query so the two cannot look in
    /// different places.
    std::vector<uint8_t> readTextureFile(const std::string& path,
                                         std::string& resolvedOut);

    /// Labels whose glyphs are wider than the rect they were given.
    void reportOverflowingText(WidgetTree& tree);
    /// Labels holding a coin amount with a letter on the end of it.
    void reportLetteredAmounts(WidgetTree& tree);

    /// Draw a string that may carry WoW's inline colour markup, as runs.
    /// wrapWidth of zero draws one line, which is what an auto-sized label
    /// and a tooltip row want; a positive one breaks the text to fit.
    /// Answers the height it drew, in pixels.
    float drawMarkupText(ImDrawList* dl, ImFont* font, float size, ImVec2 at,
                        uint32_t fallback, float alpha, const std::string& text,
                        float wrapWidth = 0.0f, bool nonSpaceWrap = false,
                        const char* justifyH = nullptr, bool forceColor = false,
                        WidgetTree* linkSink = nullptr, uint32_t linkOwner = 0);
    /// A SimpleHTML page holding a document rather than a string: its blocks
    /// stacked down the frame, text justified per block and each picture on a
    /// line of its own. See simple_html.hpp.
    void drawSimpleHtml(ImDrawList* dl, WidgetTree& tree, const Widget& w,
                        float ws, float x0, float y0, float x1);
    /// Screen height and interface scale of the pass in flight, so a link rect
    /// can be filed in the coordinates the click will arrive in.
    float linkScreenH_ = 0.0f;
    float linkScale_ = 1.0f;
    /// scale is pixels per interface unit. The rect arrives in pixels, but a
    /// backdrop's insets and edge size are authored in units like everything
    /// else, so they have to make the same trip or a border comes out the
    /// wrong thickness on any display that is not 768 pixels tall.
    void drawBackdrop(ImDrawList* dl, const Widget& w, float scale,
                      float x0, float y0, float x1, float y1);
    void drawStatusBar(ImDrawList* dl, const Widget& w,
                       float x0, float y0, float x1, float y1);
    void drawSlider(ImDrawList* dl, const Widget& w,
                    float x0, float y0, float x1, float y1);
    /// One of a colour picker's four regions: the hue-saturation wheel, the
    /// brightness bar, or either thumb. None of them has art on disk - the
    /// wheel and the bar are generated from the colour `picker` holds, and the
    /// thumbs are placed by it rather than anchored, since where they belong is
    /// the answer rather than the question.
    void drawColorPicker(ImDrawList* dl, const WidgetTree& tree, const Widget& w,
                         const Widget& picker, float screenH,
                         float x0, float y0, float x1, float y1);
    void drawThumb(ImDrawList* dl, const Widget& w,
                   float x0, float y0, float x1, float y1);
    void drawCooldown(ImDrawList* dl, const Widget& w,
                      float x0, float y0, float x1, float y1);

    pipeline::AssetManager* assets_ = nullptr;
    rendering::VkContext* vkCtx_ = nullptr;
    rendering::CpuStageTimes* stageTimes_ = nullptr;
    std::unique_ptr<UiTextureCache> textureCache_;
    std::future<std::vector<InterfaceArt>> artDiscovery_;
    bool discoveryStarted_ = false;
    bool preloadReported_ = false;
    size_t layoutTextureCount_ = 0;
    uint32_t prefetchWidgetCursor_ = 1;
    /// The cached set for a path, or null when nothing is cached for it -
    /// which is different from a cached kMissing, and both callers care.
    ///
    [[nodiscard]] const VkDescriptorSet* cachedTexture(const std::string& path,
                                                       bool add) const;
    /// Image dimensions by path, including the ones that could not be read -
    /// stored as zero so a missing file is looked for once and not once a frame.
    std::unordered_map<std::string, std::pair<float, float>> textureSizes_;
};

} // namespace ui
} // namespace wowee
