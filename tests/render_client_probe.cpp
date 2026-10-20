// Native GPU/lifetime tests of the client's real previews/maps, without a realm.
// Built only with WOWEE_BUILD_RENDER_CLIENT_PROBE=ON.
#include "core/application.hpp"
#include "core/window.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/minimap.hpp"
#include "rendering/frame_scene.hpp"
#include "rendering/world_map/world_map_facade.hpp"
#include "rendering/imgui_texture.hpp"
#include "rendering/frame_sink.hpp"
#include "rendering/vk_context.hpp"
#include "ui/map_window.hpp"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <algorithm>
#include <thread>
#include <filesystem>
#include <stb_image.h>

using namespace wowee;
namespace {
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}
class Capture final : public rendering::FrameSink {
public:
    explicit Capture(VkExtent2D extent) : extent_(extent) {}
    bool frameDue(int64_t* pts) const override { onProducer(); *pts = 0; return !taken_; }
    void frameTaken(int64_t) override { onProducer(); taken_ = true; }
    void frameDropped() override { onProducer(); throw std::runtime_error("Probe frame capture dropped"); }
    double elapsedSeconds() const override { return 0; }
    void submit(Frame frame) override {
        onProducer();
        pixels.resize(static_cast<size_t>(extent_.width) * extent_.height * 4);
        for (uint32_t y = 0; y < extent_.height; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * extent_.width * 4,
                        frame.bgra + static_cast<size_t>(y) * frame.stride, extent_.width * 4);
        frame.release();
    }
    std::vector<uint8_t> pixels;
private:
    void onProducer() const { require(std::this_thread::get_id() == owner_, "Capture callback ran outside producer"); }
    std::thread::id owner_ = std::this_thread::get_id();
    VkExtent2D extent_;
    bool taken_ = false;
};
void record(rendering::Renderer& renderer, const rendering::FrameInputs& frame) {
    renderer.beginFrame(frame);
    if (frame.worldInView) renderer.renderWorld();
    renderer.endFrame();
}
std::vector<uint8_t> capture(rendering::Renderer& renderer, const rendering::FrameInputs& frame) {
    const auto extent = renderer.getVkContext()->getSwapchainExtent();
    Capture result(extent);
    require(renderer.beginFrameCapture(&result, extent.width, extent.height), "Probe capture did not start");
    auto captured = frame;
    captured.capture = renderer.extractCaptureRequest();
    record(renderer, captured);
    renderer.flushFrameCapture();
    renderer.endFrameCapture();
    require(!result.pixels.empty(), "Probe capture returned no pixels");
    return result.pixels;
}
template<class Draw>
rendering::FrameInputs extract(rendering::Renderer& renderer, Draw&& draw) {
    auto& io = ImGui::GetIO();
    const auto extent = renderer.getVkContext()->getSwapchainExtent();
    io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    io.DeltaTime = 1.0f / 30.0f;
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    auto frame = renderer.extractFrameInputs();
    frame.interface = rendering::extractImGuiFrame(*renderer.getVkContext());
    return frame;
}
void previewTest(core::Application& app) {
    auto& renderer = *app.getRenderer();
    auto preview = std::make_shared<rendering::CharacterPreview>();
    require(preview->initialize(app.getAssetManager(), 160, 200), "Preview initialization failed");
    require(preview->loadCreature("Creature\\Wolf\\Wolf.m2",
        {{11, "Creature\\Wolf\\WolfSkinBlack.blp"}}), "Preview creature did not load");
    renderer.registerComposite(preview);
    preview->update(0);
    preview->requestComposite();
    record(renderer, renderer.extractFrameInputs());
    require(preview->getTextureId() != VK_NULL_HANDLE, "Preview image was not made ready");
    const auto image = preview->getTextureId();
    auto imageDraw = [image] {
        ImGui::GetForegroundDrawList()->AddImage(
            static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(image)), ImVec2(16, 16), ImVec2(176, 216));
    };
    preview->requestComposite();
    const auto before = capture(renderer, extract(renderer, imageDraw));
    preview->requestComposite();
    auto held = extract(renderer, imageDraw);
    preview->rotate(110);
    preview->update(0);
    require(capture(renderer, held) == before, "Rotating the live preview changed the held frame");
    preview->requestComposite();
    const auto after = capture(renderer, extract(renderer, imageDraw));
    require(after != before, "Next extraction ignored the rotated preview");
    preview->requestComposite();
    held = extract(renderer, imageDraw);
    preview->getCharacterRenderer()->clear();
    renderer.unregisterComposite(preview.get());
    preview.reset();
    require(capture(renderer, held) == after, "Clearing/destroying the preview invalidated its held frame");
    held = rendering::FrameInputs();
    record(renderer, renderer.extractFrameInputs());
    std::puts("PASS: preview pose mutation, model removal and owner destruction");
}
void mapTest(core::Application& app) {
    auto& renderer = *app.getRenderer();
    auto map = std::make_shared<rendering::world_map::WorldMapFacade>();
    require(map->initialize(renderer.getVkContext(), app.getAssetManager()), "World map initialization failed");
    map->setPersistent(true);
    map->setMapName("Northrend");
    renderer.registerComposite(map);
    const auto extent = renderer.getVkContext()->getSwapchainExtent();
    auto draw = [&] { map->render({5800, 600, 650}, extent.width, extent.height, 0); };
    for (int i = 0; i < 4; ++i) record(renderer, extract(renderer, draw));
    auto held = extract(renderer, draw);
    const auto before = capture(renderer, held);
    map->setMapName("Kalimdor");
    (void)extract(renderer, draw); // replace the live zone/overlay lists and textures
    require(capture(renderer, held) == before, "Changing map data changed a held composite");
    const auto after = capture(renderer, extract(renderer, draw));
    require(after != before, "Next extraction ignored the changed world map");
    renderer.unregisterComposite(map.get());
    map.reset();
    require(capture(renderer, held) == before, "Destroying the map owner invalidated its held frame");
    held = rendering::FrameInputs();
    record(renderer, renderer.extractFrameInputs());
    std::puts("PASS: world map source replacement, texture retirement and owner destruction");
}
void mapWindowTest(core::Application& app) {
    auto& renderer = *app.getRenderer();
    ui::MapWindow map;
    require(map.open(app.getWindow()->getSDLWindow(), &renderer, renderer.getVkContext(),
                     app.getAssetManager(), app.getUIManager()), "Map window did not open");
    map.buildFrame(true, {5800, 600, 650}, 0, 4395);
    auto frame = renderer.extractFrameInputs();
    frame.auxiliaryInterface = map.extractFrame();
    require(bool(frame.auxiliaryInterface), "Map window did not extract a frame");
    record(renderer, frame);
    SDL_Delay(40);
    map.buildFrame(true, {5800, 600, 650}, 0, 4395);
    auto held = renderer.extractFrameInputs();
    held.auxiliaryInterface = map.extractFrame();
    map.close();
    record(renderer, held); // a closed target cancels its captured request safely
    std::puts("PASS: independent map interface and close with a held auxiliary frame");
}
void minimapTest(core::Application& app) {
    auto& renderer = *app.getRenderer();
    require(renderer.initializeRenderers(app.getAssetManager(), "Northrend"), "Minimap world renderer setup failed");
    require(renderer.getMinimap() != nullptr, "Minimap renderer was not initialized");
    renderer.setWorldInView(true);
    auto& map = *renderer.getMinimap();
    map.setEnabled(true);
    map.setMapName("Northrend");
    renderer.getCharacterPosition() = {5800, 600, 650};
    renderer.getCamera()->setPosition({5800, 600, 650});
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto held = renderer.extractFrameInputs();
    while (!held.scene->minimap.enabled && std::chrono::steady_clock::now() < limit) {
        SDL_Delay(5);
        held = renderer.extractFrameInputs();
    }
    require(held.scene->minimap.enabled, "Minimap background tiles did not become ready");
    require(std::any_of(held.scene->minimap.tiles->images.begin(), held.scene->minimap.tiles->images.end(),
        [](const auto& tile) { return bool(tile); }), "Minimap fixture has no actual map textures");
    const auto before = capture(renderer, held);
    map.setViewRadius(100);
    map.setSquareShape(true);
    map.setScreenRect(20, 40, 220, 180);
    require(capture(renderer, held) == before, "Minimap controls changed a held frame");
    const auto after = capture(renderer, renderer.extractFrameInputs());
    require(after != before, "Next extraction ignored minimap controls");
    map.setMapName("Kalimdor");
    (void)renderer.extractFrameInputs();
    require(capture(renderer, held) == before, "Minimap map replacement changed held tiles");
    map.setEnabled(false);
    renderer.setWorldInView(false);
    std::puts("PASS: asynchronous minimap tiles and frozen display/map controls");
}
void resultsTest(core::Application& app) {
    auto& renderer = *app.getRenderer();
    (void)renderer.takeFrameResults();
    const auto extent = renderer.getVkContext()->getSwapchainExtent();
    Capture sink(extent);
    require(renderer.beginFrameCapture(&sink, extent.width, extent.height), "Boundary capture failed to start");
    uint64_t previous = 0;
    for (int i = 0; i < 3; ++i) {
        const auto frame = renderer.extractFrameInputs();
        record(renderer, frame);
        require(sink.pixels.empty(), "Recording called the capture sink before the producer boundary");
    }
    const auto results = renderer.takeFrameResults();
    require(!sink.pixels.empty(), "Producer boundary did not deliver the completed GPU readback");
    for (const auto& result : results) {
        require(result.recording > previous, "Recording results lost their order");
        require(result.submittedFrame != 0, "Recorded GPU frame has no submission number");
        previous = result.recording;
    }
    require(renderer.takeFrameResults().empty(), "Recording result was delivered twice");
    renderer.flushFrameCapture();
    renderer.endFrameCapture();
    std::puts("PASS: numbered results and producer-only capture callbacks");
}
void screenshotTest(core::Application& app) {
    auto& renderer = *app.getRenderer();
    const auto path = std::filesystem::current_path().parent_path() / "scratch" / "render-thread" / "boundary-shot.png";
    std::filesystem::remove(path);
    require(renderer.captureScreenshot(path.string()), "Screenshot request was refused");
    auto red = [] { ImGui::GetForegroundDrawList()->AddRectFilled({16, 16}, {140, 140}, IM_COL32(255, 0, 0, 255)); };
    auto green = [] { ImGui::GetForegroundDrawList()->AddRectFilled({16, 16}, {140, 140}, IM_COL32(0, 255, 0, 255)); };
    record(renderer, extract(renderer, red));
    record(renderer, extract(renderer, green));
    record(renderer, extract(renderer, green));
    require(!std::filesystem::exists(path), "Recording performed screenshot I/O before the producer boundary");
    (void)renderer.takeFrameResults();
    int w = 0, h = 0, channels = 0;
    auto* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
    require(pixels && w > 32 && h > 32, "Completed screenshot is not a readable PNG");
    const auto* pixel = pixels + (32 * w + 32) * 4;
    const bool correct = pixel[0] > 220 && pixel[1] < 30;
    stbi_image_free(pixels);
    require(correct, "Screenshot captured a later frame or swapped its colour channels");
    const auto completed = renderer.takeScreenshotResults();
    require(completed.size() == 1 && completed.front().second, "Screenshot completion was not reported once");
    std::puts("PASS: screenshot copies its requested frame before present and saves on the producer boundary");
}
}
int main() {
    try {
        core::Application app;
        require(app.initialize(), "Client probe initialization failed");
        SDL_HideWindow(app.getWindow()->getSDLWindow());
        auto& renderer = *app.getRenderer();
        renderer.setWorldInView(false);
        renderer.setRtLightingMode(0);
        renderer.setVolumetricFogQuality(0);
        record(renderer, renderer.extractFrameInputs());
        previewTest(app);
        mapTest(app);
        minimapTest(app);
        mapWindowTest(app);
        resultsTest(app);
        screenshotTest(app);
        renderer.getVkContext()->waitForSubmittedFrames("finish native interface probe");
        std::puts("PASS: native interface snapshot lifecycle");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
