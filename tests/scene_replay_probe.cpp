// Opt-in native capture fixture. The ordinary executable never seeds it.
#include "core/application.hpp"
#include "core/frame_observer.hpp"
#include "core/coordinates.hpp"
#include "game/game_handler.hpp"
#include "game/entity_controller.hpp"
#include "game/game_events.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/item_textures.hpp"
#include "rendering/renderer.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace wowee;
namespace {
class Crowd final : public core::FrameObserver {
public:
    explicit Crowd(core::Application& app) : app_(app) {}
    void frameBegan() override {
        if (app_.getState()!=core::AppState::IN_GAME) return;
        auto* game=app_.getGameHandler();
        if (!seeded_) {
            seeded_=true; start_=std::chrono::steady_clock::now();
            const auto feet=core::coords::renderToCanonical(app_.getRenderer()->getCharacterPosition());
            game::UpdateObjectData source{};
            for (int i=0;i<8;++i) {
                game::UpdateBlock block{};
                block.guid=first_+i;
                block.objectType=game::ObjectType::PLAYER;
                block.updateType=game::UpdateType::CREATE_OBJECT;
                block.hasMovement=true;
                const auto p=core::coords::canonicalToServer(feet+glm::vec3((i%4)*2+3,(i/4)*3,0));
                block.x=p.x; block.y=p.y; block.z=p.z;
                block.fields[game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)]=1u | (1u<<8) | ((i%2)<<16);
                block.fields[game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID)]=i%2 ? 50 : 49;
                block.fields[game::fieldIndex(game::UF::UNIT_FIELD_NATIVEDISPLAYID)]=i%2 ? 50 : 49;
                block.fields[game::fieldIndex(game::UF::UNIT_FIELD_HEALTH)]=100;
                block.fields[game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH)]=100;
                block.fields[game::fieldIndex(game::UF::PLAYER_BYTES)]=0;
                block.fields[game::fieldIndex(game::UF::PLAYER_BYTES_2)]=0;
                if (i<2) block.fields[game::fieldIndex(game::UF::UNIT_FIELD_MOUNTDISPLAYID)]=2409;
                source.blocks.push_back(std::move(block));
            }
            game->getEntityController()->applyRecordedObjects(source);
            const auto items=app_.getAssetManager()->loadDBC("ItemDisplayInfo.dbc");
            uint32_t weapon=0;
            if (items) for (uint32_t row=0;row<items->getRecordCount();++row) {
                const auto art=pipeline::readItemDisplayArt(*items,row);
                if (!art.modelFile.empty() && app_.getAssetManager()->fileExists("Item\\ObjectComponents\\Weapon\\"+art.modelFile)) {
                    weapon=items->getUInt32(row,0); break;
                }
            }
            if (!weapon) throw std::runtime_error("Replay fixture needs a real weapon asset");
            for (int i=0;i<8;++i) {
                game::events::PlayerEquipmentResolved gear{};
                gear.guid=first_+i; gear.displayIds[15]=weapon; gear.inventoryTypes[15]=13;
                app_.getEventBus().publish(gear);
            }
            std::puts("SCENE FIXTURE: eight real players, two mounts and eight weapons seeded through game/source APIs");
        }
        const auto elapsed=std::chrono::duration<float>(std::chrono::steady_clock::now()-start_).count();
        if (!moved_ && elapsed>5) {
            moved_=true;
            for (int i=2;i<8;++i) {
                auto entity=game->getEntityManager().getEntity(first_+i);
                entity->startMoveTo(entity->getX()+6,entity->getY(),entity->getZ(),0,5);
            }
            app_.getEventBus().publish(game::events::UnitEmotePlayed{first_,4,true});
            std::puts("SCENE FIXTURE: sampled movement and emote started");
        }
        if (!removed_ && elapsed>6) {
            removed_=true;
            game::UpdateObjectData gone{}; gone.outOfRangeGuids={first_+7};
            game->getEntityController()->applyRecordedObjects(gone);
            std::puts("SCENE FIXTURE: one real player despawned");
        }
        if (!emoted_ && elapsed>7) {
            emoted_=true;
            app_.getEventBus().publish(game::events::UnitEmotePlayed{first_+2,4,true});
            std::puts("SCENE FIXTURE: timed emote published during capture");
        }
    }
private:
    core::Application& app_;
    const uint64_t first_=0x1234110000000000ULL;
    bool seeded_=false,moved_=false,removed_=false,emoted_=false;
    std::chrono::steady_clock::time_point start_;
};
}
int main() {
    try {
        core::Application app;
        if (!app.initialize()) throw std::runtime_error("Replay fixture could not initialize client");
        app.observeFrames(std::make_unique<Crowd>(app));
        app.run();
        std::puts("PASS: source scene capture fixture completed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"FAIL: %s\n",e.what());
        return 1;
    }
}
