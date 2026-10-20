#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <deque>
#include <list>
#include <algorithm>
#include "core/job_group.hpp"
#include <array>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;
class VkTexture;
class VkRenderTarget;

class Minimap {
public:
    Minimap();
    ~Minimap();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, int size = 200);
    void shutdown();
    void recreatePipelines();
    /// The display pipeline, which initialize() and recreatePipelines()
    /// both build.
    void buildDisplayPipeline(VkDevice device,
                              const VkPipelineShaderStageCreateInfo& vertStage,
                              const VkPipelineShaderStageCreateInfo& fragStage);

    /// Select the render pass the display quad is recorded into.
    ///
    /// The minimap draws over the finished scene, and water is the one thing
    /// that draws after it: where water leaves the scene pass for a
    /// continuation of its own, the minimap has to follow or the water sheet is
    /// painted straight over it. Same reason SwimEffects has this.
    ///
    /// A null pass means the scene pass, which is where it goes when water has
    /// not moved. Call before initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }
    void setMapName(const std::string& name);

    struct Display {
        glm::vec4 rect;
        glm::vec2 playerUV;
        float rotation = 0, arrowRotation = 0, zoomRadius = 0;
        int32_t squareShape = 0;
        float opacity = 1;
    };
    struct Tiles {
        std::array<std::shared_ptr<VkTexture>, 9> images;
        std::array<std::string, 9> hashes;
        std::shared_ptr<const std::unordered_map<std::string, std::string>> lookup;
        std::string map;
        int x = 0, y = 0;
        uint64_t revision = 0;
    };
    struct Frame { std::shared_ptr<const Tiles> tiles; Display display; bool enabled = false; };
    Frame extract(const Camera& camera, const glm::vec3& centre, int width, int height,
                  float playerOrientation, bool hasPlayerOrientation);
    void recordComposite(VkCommandBuffer cmd, const Frame& frame);
    void record(VkCommandBuffer cmd, const Frame& frame);

    void setEnabled(bool enabled) { this->enabled = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled; }
    void toggle() { enabled = !enabled; }

    void setViewRadius(float radius) { viewRadius = radius; }
    void setRotateWithCamera(bool rotate) { rotateWithCamera = rotate; }
    [[nodiscard]] bool isRotateWithCamera() const { return rotateWithCamera; }

    void setSquareShape(bool square) { squareShape = square; }
    [[nodiscard]] bool isSquareShape() const { return squareShape; }
    [[nodiscard]] float getViewRadius() const { return viewRadius; }

    void zoomIn() { viewRadius = std::max(100.0f, viewRadius - 50.0f); }
    void zoomOut() { viewRadius = std::min(800.0f, viewRadius + 50.0f); }

    void setOpacity(float opacity) { opacity_ = opacity; }

    /// Where the map is drawn, in pixels from the top-left of the window.
    ///
    /// Unset, it goes in the top-right corner at its own size, which is where
    /// this client's own interface puts it. FrameXML puts it inside a frame it
    /// owns, so when the original interface is drawing the minimap the rect of
    /// that frame is handed here instead - this is a Vulkan pass of its own
    /// rather than an image the widget renderer could draw, so the map moves to
    /// the frame rather than the frame receiving the map.
    void setScreenRect(float x, float y, float w, float h) {
        rectX_ = x; rectY_ = y; rectW_ = w; rectH_ = h; haveRect_ = true;
    }
    void clearScreenRect() { haveRect_ = false; }
    /// Where the map is actually being drawn, when something placed it.
    ///
    /// The marker pass needs this: it used to assume the corner this client
    /// puts its own minimap in, and when FrameXML draws the ring the map moves
    /// to whatever rect the Minimap widget occupies. Blips computed against
    /// the old corner land beside the map rather than on it.
    [[nodiscard]] bool hasScreenRect() const { return haveRect_; }
    [[nodiscard]] float screenRectX() const { return rectX_; }
    [[nodiscard]] float screenRectY() const { return rectY_; }
    [[nodiscard]] float screenRectW() const { return rectW_; }
    [[nodiscard]] float screenRectH() const { return rectH_; }

    [[nodiscard]] const std::string& getMapName() const { return mapName; }

private:
    void prepareTiles(int x, int y);
    void updateTileDescriptors(uint32_t frameIdx, const Tiles& tiles);

    VkContext* vkCtx = nullptr;
    /// Null until something says otherwise - see setTargetPass.
    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;
    pipeline::AssetManager* assetManager = nullptr;
    std::string mapName = "Azeroth";

    // TRS lookup: "Azeroth\map32_49" → "e7f0dea73ee6baca78231aaf4b7e772a"
    std::shared_ptr<const std::unordered_map<std::string, std::string>> trsLookup;
    core::JobGroup tileJobs_;
    core::JobGroup::Future<Tiles> pendingTiles_;
    std::shared_ptr<const Tiles> readyTiles_;
    std::string requestedMap_;
    int requestedX_ = -1, requestedY_ = -1;
    uint64_t tileRevision_ = 0, compositeRevision_ = 0;

    // Tile texture cache: hash → VkTexture, the least recently drawn let go
    // when more than MAX_TILE_CACHE tiles are loaded. The nine around the
    // player are drawn every frame, so they are never the ones let go; one
    // that is goes after the frames in flight that sample it.
    // A null texture marks a tile that failed to load.
    static constexpr size_t MAX_TILE_CACHE = 128;
    struct CachedTile {
        std::shared_ptr<VkTexture> texture;
        std::list<std::string>::iterator lru;  // tileLru_.end() for a failed tile
    };
    std::unordered_map<std::string, CachedTile> tileTextureCache;
    std::list<std::string> tileLru_;  // hashes of loaded tiles, most recently drawn first
    std::unique_ptr<VkTexture> noDataTexture;

    // Composite render target (3x3 tiles = 768x768)
    std::unique_ptr<VkRenderTarget> compositeTarget;
    static constexpr int TILE_PX = 256;
    static constexpr int COMPOSITE_PX = TILE_PX * 3;  // 768

    // Shared quad vertex buffer (6 verts, pos2 + uv2 = 16 bytes/vert)
    ::VkBuffer quadVB = VK_NULL_HANDLE;
    VmaAllocation quadVBAlloc = VK_NULL_HANDLE;

    // Descriptor resources (shared layout: 1 combined image sampler at binding 0)
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_DESC_SETS = 24;

    // Tile composite pipeline (renders into VkRenderTarget)
    VkPipeline tilePipeline = VK_NULL_HANDLE;
    VkPipelineLayout tilePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet tileDescSets[2][9] = {};  // [frameInFlight][tileSlot]

    // Display pipeline (renders into main render pass)
    VkPipeline displayPipeline = VK_NULL_HANDLE;
    VkPipelineLayout displayPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet displayDescSet = VK_NULL_HANDLE;

    int mapSize = 200;
    float viewRadius = 400.0f;
    bool enabled = true;
    bool rotateWithCamera = false;
    bool squareShape = false;
    float opacity_ = 1.0f;
    bool  haveRect_ = false;
    float rectX_ = 0.0f, rectY_ = 0.0f, rectW_ = 0.0f, rectH_ = 0.0f;

    bool hasCachedFrame = false;

    // No arrow texture: the player arrow is a triangle the display shader
    // draws from push.arrowRotation. A MinimapArrow.blp was loaded here once
    // and the members outlived the drawing - never assigned, so the teardown
    // that freed them could not run, and the two accessors that read them had
    // no callers. Kept as a note rather than as fields, because the real
    // client draws the texture and honours SetPlayerTextureWidth/Height on it,
    // which this cannot: those two are still no-ops.
};

} // namespace rendering
} // namespace wowee
