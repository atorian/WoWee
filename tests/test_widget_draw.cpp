// What WidgetRenderer::draw does to the tree before it paints: tooltips are
// sized to the lines they hold by then, because FrameXML fills one from
// handlers that run after the frame's layout.
//
// Headless: an ImGui context with its default font and no device, which is
// all the sizing asks of it - the draw lists are built and never submitted.
#include <catch_amalgamated.hpp>

#include "ui/widget_renderer.hpp"
#include "ui/widget_tree.hpp"

#include <imgui.h>

using wowee::ui::Anchor;
using wowee::ui::Widget;
using wowee::ui::WidgetKind;
using wowee::ui::WidgetRenderer;
using wowee::ui::WidgetTree;

namespace {

/// One ImGui frame, open for the life of the object.
struct ImGuiFrame {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiFrame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1024.0f, 768.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::NewFrame();
    }
    ~ImGuiFrame() {
        ImGui::EndFrame();
        ImGui::DestroyContext(context);
    }
};

} // namespace

namespace {

uint32_t addTooltip(WidgetTree& tree, int lines) {
    const uint32_t tip = tree.create(WidgetKind::Frame, tree.root(), "Tip");
    Widget* w = tree.get(tip);
    w->isTooltip = true;
    w->objectType = "GameTooltip";
    w->width = 120.0f;
    w->height = 20.0f;
    for (int i = 0; i < lines; ++i) {
        w->tooltipLines.push_back({"Thunderfury", "", {1, 1, 1, 1}, {1, 1, 1, 1}});
    }
    tree.addPoint(tip, Anchor{"BOTTOM", 0, "CENTER", 0.0f, 0.0f});
    return tip;
}

} // namespace

TEST_CASE("a tooltip filled after the layout is drawn as tall as one filled before it",
          "[widget][tooltip][draw]") {
    ImGuiFrame frame;
    WidgetRenderer renderer;
    renderer.initialize(nullptr, nullptr);

    // Six lines from the start.
    WidgetTree whole;
    const uint32_t wholeTip = addTooltip(whole, 6);
    renderer.layout(whole, 1024.0f, 768.0f);
    renderer.draw(whole, 1024.0f, 768.0f);
    const float sixLines = whole.get(wholeTip)->rectH;

    // One line when it was laid out and drawn, the way an OnEnter leaves it,
    // then five more from a handler that runs between the next layout and
    // its draw - the micro button's latency lines arrive this way every frame.
    WidgetTree late;
    const uint32_t lateTip = addTooltip(late, 1);
    renderer.layout(late, 1024.0f, 768.0f);
    renderer.draw(late, 1024.0f, 768.0f);
    const float oneLine = late.get(lateTip)->rectH;
    renderer.layout(late, 1024.0f, 768.0f);
    for (int i = 0; i < 5; ++i) {
        late.get(lateTip)->tooltipLines.push_back({"Thunderfury", "", {1, 1, 1, 1}, {1, 1, 1, 1}});
    }
    renderer.draw(late, 1024.0f, 768.0f);

    CHECK(sixLines > oneLine);
    CHECK(late.get(lateTip)->rectH == Catch::Approx(sixLines));
}
