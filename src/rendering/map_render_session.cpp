#include "rendering/map_render_session.hpp"
#include "rendering/renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/character_emitters.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/footprint_renderer.hpp"

namespace wowee::rendering {
namespace {
/// The layer's GPU pools belong to the renderer; the contents belong to this
/// scene. Its own clear() performs any necessary fence retirement.
template<class T> class LayerContents {
public:
    explicit LayerContents(T& layer) : layer_(layer) {}
    ~LayerContents() { layer_.clear(); }
    LayerContents(const LayerContents&) = delete;
    LayerContents& operator=(const LayerContents&) = delete;
private:
    T& layer_;
};

struct TerrainStreamingSession {
    // Destruction stops/destroys the manager before its draw layers retire.
    LayerContents<TerrainRenderer> tiles;
    LayerContents<WaterRenderer> water;
    std::unique_ptr<TerrainManager> manager;
    explicit TerrainStreamingSession(Renderer& renderer)
        : tiles(*renderer.getTerrainRenderer()), water(*renderer.getWaterRenderer()),
          manager(renderer.takeTerrainSession()) {}
};
}

struct MapRenderSession::Impl {
    Renderer& renderer;
    std::unique_ptr<M2Renderer::InstanceSession> m2;
    std::unique_ptr<WMORenderer::InstanceSession> wmo;
    std::unique_ptr<CharacterRenderer::InstanceSession> characters;
    std::unique_ptr<CharacterEmitters::InstanceSession> emitters;
    std::unique_ptr<LayerContents<QuestMarkerRenderer>> markers;
    std::unique_ptr<LayerContents<FootprintRenderer>> footprints;
    std::unique_ptr<TerrainStreamingSession> terrain;
    explicit Impl(Renderer& r) : renderer(r) {}
};

MapRenderSession::MapRenderSession(Renderer& renderer, core::JobGroup::Token token)
    : impl_(std::make_unique<Impl>(renderer)) {
    renderer.setMapJobToken(std::move(token));
    attach();
}

MapRenderSession::~MapRenderSession() {
    stopStreaming();
    if (impl_->terrain) impl_->renderer.releaseTerrainSession(impl_->terrain->manager.get());
    impl_->renderer.setMapJobToken({});
}

void MapRenderSession::stopStreaming() {
    if (impl_->terrain) impl_->terrain->manager->stopWorkers();
}

void MapRenderSession::attach() {
    auto& r = impl_->renderer;
    if (!impl_->m2 && r.getM2Renderer())
        impl_->m2 = std::make_unique<M2Renderer::InstanceSession>(*r.getM2Renderer());
    if (!impl_->wmo && r.getWMORenderer())
        impl_->wmo = std::make_unique<WMORenderer::InstanceSession>(*r.getWMORenderer());
    if (!impl_->characters && r.getCharacterRenderer())
        impl_->characters = std::make_unique<CharacterRenderer::InstanceSession>(*r.getCharacterRenderer());
    if (!impl_->emitters && r.getCharacterEmitters())
        impl_->emitters = std::make_unique<CharacterEmitters::InstanceSession>(*r.getCharacterEmitters());
    if (!impl_->markers && r.getQuestMarkerRenderer())
        impl_->markers = std::make_unique<LayerContents<QuestMarkerRenderer>>(*r.getQuestMarkerRenderer());
    if (!impl_->footprints && r.getFootprintRenderer())
        impl_->footprints = std::make_unique<LayerContents<FootprintRenderer>>(*r.getFootprintRenderer());
    if (!impl_->terrain && r.getTerrainManager())
        impl_->terrain = std::make_unique<TerrainStreamingSession>(r);
}
}
