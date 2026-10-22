#include <catch_amalgamated.hpp>
#include "rendering/scene_spatial_index.hpp"
#include "rendering/rt_region.hpp"
#include <random>
#include <set>

using namespace wowee::rendering;

namespace {
RtAabb box(glm::vec3 centre, glm::vec3 radius) {
    RtAabb b; b.grow(centre-radius); b.grow(centre+radius); return b;
}
std::set<uint32_t> query(SceneSpatialIndex& index, const RtRegion& region) {
    std::set<uint32_t> result;
    index.query([&](const RtAabb& b) { return region.keeps(b, true); },
                [&](uint32_t id) { REQUIRE(result.insert(id).second); });
    return result;
}
}

TEST_CASE("RT spatial query retains offscreen casters and large rotated bounds", "[scene-index]") {
    SceneSpatialIndex index;
    std::vector<RtAabb> bounds;
    std::mt19937 random(71);
    std::uniform_real_distribution<float> position(-5000,5000), radius(1,600);
    for(uint32_t id=0;id<3000;++id) {
        bounds.push_back(box({position(random),position(random),position(random)},
                             {radius(random),radius(random),radius(random)}));
        index.update(id,bounds.back());
    }
    for(int step=0;step<25;++step) {
        RtRegion region;
        region.centre={position(random),position(random),position(random)};
        region.distance=300;
        region.sunDirection=glm::normalize(glm::vec3(.7f,-.3f,.5f));
        region.sunlight=step%2==0;
        std::set<uint32_t> expected;
        for(uint32_t id=0;id<bounds.size();++id)
            if(region.keeps(bounds[id],true)) expected.insert(id);
        CHECK(query(index,region)==expected);
    }
}

TEST_CASE("Moving deleting and reusing a spatial identity leaves no old placement", "[scene-index]") {
    SceneSpatialIndex index;
    RtRegion region; region.sunlight=false; region.distance=30; region.indirectLength=0;
    index.update(8,box({-1,0,0},{2,2,2}));
    index.update(9,box({-3,0,0},{2,2,2}));
    CHECK(query(index,region)==std::set<uint32_t>{8,9});
    index.update(8,box({3000,0,0},{2,2,2}));
    CHECK(query(index,region)==std::set<uint32_t>{9});
    index.remove(9); index.remove(8);
    CHECK(query(index,region).empty());
    CHECK(index.cells()==0);
    index.update(8,box({0,0,0},{2,2,2}));
    CHECK(query(index,region)==std::set<uint32_t>{8});
    for(int step=0;step<1000;++step) index.update(8,box({float(step*200),0,0},{1,1,1}));
    CHECK(index.cells()==1);
    index.remove(8);
    CHECK(index.cells()==0);
}

TEST_CASE("A small region does not inspect every far-away placement", "[scene-index]") {
    SceneSpatialIndex index;
    for(uint32_t id=0;id<10000;++id)
        index.update(id,box({5000.f+float(id%20),5000.f+float(id/20%20),float(id%5)}, {1,1,1}));
    index.update(10000,box({0,0,0},{1,1,1}));
    RtRegion region; region.distance=30; region.sunlight=false;
    size_t tests=0;
    std::vector<uint32_t> hits;
    index.query([&](const RtAabb& b) { ++tests; return region.keeps(b,true); },
                [&](uint32_t id) { hits.push_back(id); });
    REQUIRE(hits==std::vector<uint32_t>{10000});
    CHECK(tests<100); // work bound for this query, independent of insertion order
}
