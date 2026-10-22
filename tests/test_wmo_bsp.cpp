#include <catch_amalgamated.hpp>
#include "pipeline/wmo_loader.hpp"
#include "collision/wmo_bsp_query.hpp"
#include <cstring>

namespace {
using Bytes = std::vector<uint8_t>;
template<class T> void put(Bytes& bytes, T value) {
    const auto at = bytes.size();
    bytes.resize(at+sizeof(T));
    std::memcpy(bytes.data()+at, &value, sizeof(T));
}
void chunk(Bytes& out, uint32_t id, const Bytes& body) {
    put(out,id); put(out,uint32_t(body.size()));
    out.insert(out.end(),body.begin(),body.end());
}
Bytes groupFile(bool brokenReference=false, bool cycle=false) {
    Bytes group(68,0), vertices, indices, flags, nodes, references;
    for (float z : {-.4f, 0.f, .4f}) {
        for (auto p : {glm::vec3(-1,-1,z), glm::vec3(1,-1,z), glm::vec3(0,1,z)}) {
            put(vertices,p.x); put(vertices,p.y); put(vertices,p.z);
        }
        put(flags,uint8_t(0x20)); put(flags,uint8_t(0));
    }
    for (uint16_t i=0; i<9; ++i) put(indices,i);
    put(nodes,uint16_t(cycle ? 0 : 4));
    put(nodes,uint16_t(cycle ? 0 : 0xffff));
    put(nodes,uint16_t(0xffff)); put(nodes,uint16_t(2));
    put(nodes,uint32_t(0)); put(nodes,0.f);
    put(references,uint16_t(brokenReference ? 3 : 2)); put(references,uint16_t(0));
    chunk(group,0x4d4f5654,vertices); // MOVT
    chunk(group,0x4d4f5649,indices); // MOVI
    chunk(group,0x4d4f5059,flags); // MOPY
    chunk(group,0x4d4f424e,nodes); // MOBN
    chunk(group,0x4d4f4252,references); // MOBR
    Bytes file;
    chunk(file,0x4d4f4750,group);
    return file;
}
}

TEST_CASE("A loaded WMO supplies only authored BSP faces, in their authored order", "[collision][wmo]") {
    wowee::pipeline::WMOModel model;
    model.groups.resize(1);
    REQUIRE(wowee::pipeline::WMOLoader::loadGroup(groupFile(), model, 0));
    const auto& group = model.groups.front();
    REQUIRE(group.collisionBsp);
    std::vector<glm::vec3> vertices;
    for (const auto& v : group.vertices) vertices.push_back(v.position);
    const auto hit = wowee::collision::queryWmoBsp(*group.collisionBsp, vertices,
        group.indices, group.triFlags, {-2,-2,-2}, {2,2,2}, {-1,-1,-1}, {1,1,1}, 0x100111);
    CHECK(hit.faces == std::vector<uint16_t>{2,0});
    CHECK_FALSE(hit.overflow);
}

TEST_CASE("A loaded WMO refuses broken collision topology", "[collision][wmo]") {
    wowee::pipeline::WMOModel model;
    model.groups.resize(1);
    SECTION("MOBR cannot refer past MOVI") {
        CHECK_FALSE(wowee::pipeline::WMOLoader::loadGroup(groupFile(true), model, 0));
    }
    SECTION("MOBN cannot loop into its parent") {
        CHECK_FALSE(wowee::pipeline::WMOLoader::loadGroup(groupFile(false,true), model, 0));
    }
    CHECK_FALSE(model.groups.front().collisionBsp);
}
