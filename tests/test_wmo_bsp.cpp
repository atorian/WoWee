#include <catch_amalgamated.hpp>
#include "pipeline/wmo_loader.hpp"
#include "collision/wmo_bsp_query.hpp"
#include "collision/wmo_shape.hpp"
#include "collision/liquid_facets.hpp"
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

namespace {
Bytes liquidOnlyGroupFile(bool truncated=false,bool solid=false) {
    Bytes liquid;
    for (uint32_t n:{2u,2u,1u,1u}) put(liquid,n);
    put(liquid,10.0f);put(liquid,20.0f);put(liquid,90.0f);put(liquid,uint16_t(0));
    for (int i=0;i<4;++i) { put(liquid,uint32_t(0));put(liquid,0.0f); }
    put(liquid,uint8_t(0x80)); // Visual sharing bit does not remove collision.
    Bytes body(68,0),file;
    if (solid) {
        Bytes vertices,indices;
        for (const auto& v:{glm::vec3(0,0,0),glm::vec3(1,0,0),glm::vec3(0,1,0)}) {
            put(vertices,v.x);put(vertices,v.y);put(vertices,v.z);
        }
        for (uint16_t i:{0,1,2}) put(indices,i);
        chunk(body,0x4d4f5654,vertices);chunk(body,0x4d4f5649,indices);
    }
    if (truncated) liquid.pop_back();
    chunk(body,0x4d4c4951,liquid);chunk(file,0x4d4f4750,body);
    return file;
}
}

TEST_CASE("A WMO liquid-only group retains authored collision geometry", "[collision][wmo]") {
    const auto file=liquidOnlyGroupFile();
    wowee::pipeline::WMOModel model;model.groups.resize(1);
    REQUIRE(wowee::pipeline::WMOLoader::loadGroup(file,model,0));
    const auto shape=wowee::collision::WmoGroupShape::build(model.groups[0]);
    REQUIRE(shape->original);
    REQUIRE(shape->original->liquid);
    const auto& source=*shape->original->liquid;
    REQUIRE(source.valid);
    const wowee::collision::WmoLiquidGrid grid{source.xVerts,source.yVerts,source.xTiles,source.yTiles,
        source.corner,source.vertices,source.tiles,source.liquidType,4};
    const auto hit=wowee::collision::queryWmoLiquidFacets(grid,{9,19,-1},{16,26,1},0x20000,glm::mat4(1),42,
        wowee::collision::FacetNormalization::X87);
    REQUIRE(hit);
    REQUIRE(hit->facets.size()==2);
    CHECK(hit->facets[0].owner==42);
    CHECK(hit->facets[0].vertices[0].z==0.0f);
    CHECK(hit->facets[0].plane.normal.z==1.0f);
}

TEST_CASE("A truncated WMO liquid cannot acquire invented wet cells", "[collision][wmo]") {
    wowee::pipeline::WMOModel model;model.groups.resize(1);
    CHECK_FALSE(wowee::pipeline::WMOLoader::loadGroup(liquidOnlyGroupFile(true,true),model,0));
}
