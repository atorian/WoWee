#include <catch_amalgamated.hpp>
#include "rendering/rt_scene.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <map>
#include <set>

using namespace wowee::rendering;
namespace {
RtScene::MeshSource triangle() {
    return {{{-1,-1,0},{1,-1,0},{0,1,1}}, {0,1,2}, {1.f}};
}
glm::mat4 at(float x, float y=0) { return glm::translate(glm::mat4(1),glm::vec3(x,y,0)); }
// Interpret the public handoff just as a consumer does, including repeated
// journals after a dropped frame. No access to RtScene's internal containers.
struct Consumer {
    uint64_t serial=0;
    std::set<uint32_t> meshes;
    std::map<uint32_t,std::pair<uint32_t,glm::mat4>> instances;
    void accept(const RtScene::Changes& changes) {
        for(const auto& c:changes) {
            if(c.serial<=serial) continue;
            using K=RtScene::Change::Kind;
            switch(c.kind) {
            case K::AddMesh: REQUIRE(c.source); REQUIRE(meshes.insert(c.id).second); break;
            case K::RemoveMesh:
                for(const auto& [id,inst]:instances) REQUIRE(inst.first!=c.id);
                REQUIRE(meshes.erase(c.id)==1); break;
            case K::AddInstance:
                REQUIRE(meshes.contains(c.mesh));
                REQUIRE(instances.emplace(c.id,std::make_pair(c.mesh,c.transform)).second); break;
            case K::RemoveInstance: REQUIRE(instances.erase(c.id)==1); break;
            case K::MoveInstance: REQUIRE(instances.contains(c.id)); instances.at(c.id).second=c.transform; break;
            }
            serial=c.serial;
        }
    }
};
}

TEST_CASE("RT publication updates a moved instance without reading the resident world", "[rt-publication]") {
    RtScene scene; Consumer consumer;
    auto mesh=scene.addMesh(triangle());
    auto nearInstance=scene.addInstance(mesh,at(0));
    for(int i=0;i<10000;++i) scene.addInstance(mesh,at(5000+float(i%50),5000+float(i/50)));
    RtRegion region; region.distance=50; region.sunlight=false;
    consumer.accept(scene.extractChanges(region));
    REQUIRE(consumer.instances.size()==1);
    scene.setInstanceTransform(nearInstance,at(10)); scene.setInstanceTransform(nearInstance,at(20));
    consumer.accept(scene.extractChanges(region));
    CHECK(consumer.instances.at(nearInstance).second==at(20));
    CHECK(scene.extractionStats().testedInstances==1);
    consumer.accept(scene.extractChanges(region));
    CHECK(scene.extractionStats().testedInstances==0);
    region.centre.x+=1;
    consumer.accept(scene.extractChanges(region));
    CHECK(scene.extractionStats().testedInstances<100);
}

TEST_CASE("RT camera movement includes offscreen sun casters and retires the old region", "[rt-publication]") {
    RtScene scene; Consumer consumer;
    auto mesh=scene.addMesh(triangle());
    const auto nearInstance=scene.addInstance(mesh,at(0));
    const auto caster=scene.addInstance(mesh,at(1000));
    const auto farInstance=scene.addInstance(mesh,at(5000));
    RtRegion region; region.distance=50; region.indirectLength=0; region.sunDirection={1,0,0};
    consumer.accept(scene.extractChanges(region));
    REQUIRE(consumer.instances.size()==2);
    CHECK(consumer.instances.contains(nearInstance)); CHECK(consumer.instances.contains(caster));
    region.sunlight=false;
    consumer.accept(scene.extractChanges(region));
    REQUIRE(consumer.instances.size()==1); CHECK(consumer.instances.contains(nearInstance));
    region.centre.x=5000;
    consumer.accept(scene.extractChanges(region));
    REQUIRE(consumer.instances.size()==1); CHECK(consumer.instances.contains(farInstance));
    region.enabled=false;
    consumer.accept(scene.extractChanges(region));
    CHECK(consumer.instances.empty()); CHECK(consumer.meshes.empty());
    region.enabled=true;
    consumer.accept(scene.extractChanges(region));
    REQUIRE(consumer.instances.size()==1); CHECK(consumer.instances.contains(farInstance));
}

TEST_CASE("Unconsumed RT snapshots preserve replacements and mesh lifetime", "[rt-publication]") {
    RtScene scene; Consumer consumer;
    auto mesh=scene.addMesh(triangle());
    auto id=scene.addInstance(mesh,at(1));
    auto second=scene.addInstance(mesh,at(2));
    auto first=scene.extractChanges();
    scene.removeInstance(id);
    auto replacement=scene.addInstance(mesh,at(1));
    REQUIRE(replacement==id); // exercise same address, mesh and transform
    auto next=scene.extractChanges();
    consumer.accept(first);
    const auto serial=consumer.serial;
    consumer.accept(next);
    CHECK(consumer.serial>serial);
    CHECK(consumer.instances.size()==2);
    scene.removeInstance(id);
    consumer.accept(scene.extractChanges());
    CHECK(consumer.meshes.contains(mesh));
    scene.removeInstance(second); scene.removeMesh(mesh);
    auto newMesh=scene.addMesh(triangle());
    scene.addInstance(newMesh,at(9));
    // This consumer deliberately never saw the intermediate empty scene.
    consumer.accept(scene.extractChanges());
    REQUIRE(consumer.instances.size()==1);
    CHECK(consumer.instances.begin()->second.second==at(9));
    Consumer delayed; delayed.accept(first);
    CHECK(delayed.instances.size()==2);
    delayed.accept(scene.extractChanges());
    CHECK(delayed.instances==consumer.instances);
}
