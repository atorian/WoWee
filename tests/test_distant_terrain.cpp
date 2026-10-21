#include <catch_amalgamated.hpp>
#include "pipeline/distant_terrain.hpp"
#include <cstring>
#include <algorithm>
namespace {
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){std::memcpy(b.data()+p,&v,4);}
std::vector<uint8_t> sample() {
    std::vector<uint8_t> b(8+16384+8+1090+8+32);
    std::memcpy(b.data(),"FOAM",4);put32(b,4,16384);
    put32(b,8+(23*64+38)*4,8+16384);
    size_t p=8+16384;std::memcpy(b.data()+p,"ERAM",4);put32(b,p+4,1090);
    for(size_t i=0;i<545;++i){int16_t h=1200;std::memcpy(b.data()+p+8+i*2,&h,2);}
    p+=1098;std::memcpy(b.data()+p,"OHAM",4);put32(b,p+4,32);
    return b;
}
}
TEST_CASE("distant map reads actual tile placement and authored height", "[terrain][distant]") {
    auto m=wowee::pipeline::readDistantMap(sample());REQUIRE(m.valid);REQUIRE(m.tiles.size()==1);
    auto mesh=wowee::pipeline::distantTileMesh(m.tiles.front());
    CHECK(mesh.min.z==1200);CHECK(mesh.max.z==1200);
    CHECK(mesh.min.x>4200);CHECK(mesh.max.x<4900);
    CHECK(mesh.min.y < -3600);CHECK(mesh.max.y > -3201);
    CHECK(mesh.lods.back().count < mesh.lods.front().count/8);
    CHECK(mesh.lods.back().error==0);
}
TEST_CASE("distant terrain preserves a peak when reducing detail would flatten it", "[terrain][distant]") {
    auto bytes=sample();int16_t peak=1900;std::memcpy(bytes.data()+8+16384+8+(289+8*16+8)*2,&peak,2);
    auto map=wowee::pipeline::readDistantMap(bytes);auto mesh=wowee::pipeline::distantTileMesh(map.tiles.front());
    CHECK(mesh.max.z==1900);CHECK(mesh.lods[1].error>=699);
}
TEST_CASE("distant terrain does not seal authored holes", "[terrain][distant]") {
    auto bytes=sample();uint16_t hole=1;std::memcpy(bytes.data()+bytes.size()-32,&hole,2);
    auto map=wowee::pipeline::readDistantMap(bytes);auto mesh=wowee::pipeline::distantTileMesh(map.tiles.front());
    auto full=wowee::pipeline::distantTileMesh(wowee::pipeline::readDistantMap(sample()).tiles.front());
    CHECK(mesh.lods[0].count < full.lods[0].count);CHECK(mesh.lods[1].count==0);
    for(auto i:mesh.indices)CHECK(i<mesh.vertices.size());
}
TEST_CASE("bad distant tile offsets fail rather than drawing fabricated ground", "[terrain][distant]") {
    auto bytes=sample();put32(bytes,8+(23*64+38)*4,0xfffffff0);
    CHECK_FALSE(wowee::pipeline::readDistantMap(bytes).valid);
}
namespace aerial {
using vec3=glm::vec3;using vec4=glm::vec4;
using glm::exp;using glm::pow;using glm::max;using glm::abs;using glm::mix;using glm::clamp;using glm::dot;using glm::normalize;
#include "../assets/shaders/aerial_perspective.glsli"
}
TEST_CASE("day haze approaches lit air while the night's authored tint survives", "[fog][aerial]") {
    using namespace aerial;
    vec3 violet(.4,.12,.42),neutral(.75,.72,.68),sunlight(.8,.65,.4);
    vec3 day=aerialScatteringColor(vec3(0,1,0),violet,neutral,sunlight,vec4(0,1,0,1));
    CHECK(day.r>violet.r);CHECK(day.g>violet.g*3);
    CHECK(aerialScatteringColor(vec3(0,1,0),violet,neutral,sunlight,vec4(0,1,0,0))==violet);
}

#include "rendering/terrain_placement_refs.hpp"
TEST_CASE("a landmark survives unloading one of the tiles sharing it", "[terrain][distant]") {
    wowee::rendering::TerrainPlacementRefs refs;
    refs.insert(1001,17);
    auto otherTile=refs.retain(1001);
    CHECK_FALSE(refs.release(17));
    CHECK(refs.at(1001)==otherTile);
    CHECK(refs.release(otherTile));
    CHECK(refs.count(1001)==0);
    refs.insert(1001,42); // revisiting the region creates a fresh placement
    CHECK(refs.at(1001)==42);
}

TEST_CASE("promoting a shared far hull retains its detailed resources until the last reference", "[terrain][distant]") {
    wowee::rendering::TerrainPlacementRefs refs;
    refs.insert(27,200);
    auto near=refs.retain(27);
    CHECK_FALSE(refs.hasDetail(near));
    refs.markDetail(near);
    CHECK_FALSE(refs.release(200));
    CHECK(refs.hasDetail(near));
    CHECK(refs.release(near));
    refs.insert(27,201);
    CHECK_FALSE(refs.hasDetail(201));
}

TEST_CASE("WMO attachments belong to the shared hull across tile handoffs", "[terrain][distant]") {
    wowee::rendering::TerrainPlacementRefs refs;
    refs.insert(500,20); refs.attach(20,71); refs.attach(20,72);
    refs.retain(500);
    std::vector<uint32_t> attached;
    CHECK(refs.release(std::vector<uint32_t>{20},&attached).empty());
    CHECK(attached.empty());
    CHECK(refs.release(std::vector<uint32_t>{20},&attached)==std::vector<uint32_t>{20});
    CHECK(attached==std::vector<uint32_t>{71,72});
}
