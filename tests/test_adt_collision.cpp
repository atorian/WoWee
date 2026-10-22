#include <catch_amalgamated.hpp>
#include "pipeline/adt_loader.hpp"
#include <cstring>
#include <vector>

namespace {
void put(std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
    REQUIRE(at+4<=bytes.size());std::memcpy(bytes.data()+at,&value,4);
}
void chunk(std::vector<uint8_t>& bytes,uint32_t magic,const std::vector<uint8_t>& data) {
    const auto start=bytes.size();bytes.resize(start+8+data.size());
    put(bytes,start,magic);put(bytes,start+4,uint32_t(data.size()));
    std::memcpy(bytes.data()+start+8,data.data(),data.size());
}
std::vector<uint8_t> terrain(bool truncated=false) {
    std::vector<uint8_t> bytes,header(128),mhdr(64);
    put(mhdr,0,1);chunk(bytes,0x4d484452,mhdr);
    chunk(bytes,0x4d444446,std::vector<uint8_t>(3*36)); // Three MDDF placements.
    chunk(bytes,0x4d4f4446,std::vector<uint8_t>(2*64)); // Two MODF placements.
    put(header,16,3);put(header,56,2);put(header,32,136);
    std::vector<uint8_t> refs(5*4);
    put(refs,0,2);put(refs,4,0);put(refs,8,1);put(refs,12,1);put(refs,16,0);
    if (truncated) refs.resize(16);
    chunk(header,0x4d435246,refs);
    chunk(bytes,0x4d434e4b,header);
    std::vector<uint8_t> flight(36);
    for (int i=0;i<18;++i) { const int16_t v=i<9 ? int16_t(700+i) : int16_t(-100-i);std::memcpy(flight.data()+i*2,&v,2); }
    chunk(bytes,0x4d46424f,flight);
    return bytes;
}
}
TEST_CASE("ADT retains authored collision references and flight bounds", "[adt][collision]") {
    const auto data=wowee::pipeline::ADTLoader::load(terrain());
    CHECK(data.collisionDataValid);
    CHECK(data.chunks[0].doodadReferences==std::vector<uint32_t>{2,0,1});
    CHECK(data.chunks[0].wmoReferences==std::vector<uint32_t>{1,0});
    REQUIRE(data.flightBounds);
    CHECK((*data.flightBounds)[0]==700);
    CHECK((*data.flightBounds)[8]==708);
    CHECK((*data.flightBounds)[9]==-109);
    CHECK((*data.flightBounds)[17]==-117);
}
TEST_CASE("a truncated reference list cannot become an apparently clear chunk", "[adt][collision]") {
    const auto data=wowee::pipeline::ADTLoader::load(terrain(true));
    CHECK_FALSE(data.collisionDataValid);
    CHECK(data.chunks[0].doodadReferences.empty());
    CHECK(data.chunks[0].wmoReferences.empty());
}

TEST_CASE("ADT flight bounds obey MHDR participation and reject missing data", "[adt][collision]") {
    auto bytes=terrain();
    SECTION("an unflagged MFBO does not participate") {
        put(bytes,8,0);
        const auto data=wowee::pipeline::ADTLoader::load(bytes);
        CHECK(data.collisionDataValid);
        CHECK_FALSE(data.flightBounds);
    }
    SECTION("a flagged missing MFBO is a collision load failure") {
        bytes.resize(bytes.size()-44);
        const auto data=wowee::pipeline::ADTLoader::load(bytes);
        CHECK_FALSE(data.collisionDataValid);
    }
    SECTION("an absent referenced placement is a collision load failure") {
        // Locate the authored MCRF payload and replace its first MDDF ordinal.
        size_t at=0;
        for (; at+4<=bytes.size(); ++at) {
            uint32_t magic;std::memcpy(&magic,bytes.data()+at,4);
            if (magic==0x4d435246) break;
        }
        put(bytes,at+8,3);
        const auto data=wowee::pipeline::ADTLoader::load(bytes);
        CHECK_FALSE(data.collisionDataValid);
    }
}

TEST_CASE("authored liquid collision survives visual fallbacks and masks", "[adt][collision]") {
    std::vector<uint8_t> mcnk(128), liquid(804*2);
    put(mcnk,0,0x14); // Water then magma, two full SMLiquidChunk records.
    put(mcnk,0x60,136);put(mcnk,0x64,uint32_t(liquid.size()+8));
    const float ceiling=50.0f, magma=17.0f;
    std::memcpy(liquid.data()+4,&ceiling,4);
    // All-zero authored water vertices must not become maxHeight=50.
    std::fill(liquid.begin()+656,liquid.begin()+720,0x80);
    for (size_t i=0;i<81;++i) std::memcpy(liquid.data()+804+8+i*8+4,&magma,4);
    std::fill(liquid.begin()+804+656,liquid.begin()+804+720,0x02);
    chunk(mcnk,0x4d434c51,liquid);
    std::vector<uint8_t> bytes;chunk(bytes,0x4d434e4b,mcnk);
    const auto data=wowee::pipeline::ADTLoader::load(bytes);
    REQUIRE(data.collisionDataValid);
    REQUIRE(data.collisionLiquids[0].size()==2);
    const auto& water=data.collisionLiquids[0][0];
    CHECK(water.liquidType==1);
    REQUIRE(water.legacyTiles);
    CHECK((*water.legacyTiles)[0]==0x80);
    REQUIRE(water.heights.size()==81);
    CHECK(water.heights.front()==0.0f);
    CHECK(water.heights.back()==0.0f);
    const auto& lava=data.collisionLiquids[0][1];
    CHECK(lava.liquidType==3);
    REQUIRE(lava.heights.size()==81);
    CHECK(lava.heights.front()==17.0f);
    CHECK(lava.heights.back()==17.0f);
}
TEST_CASE("MH2O collision uses authored vertex format and packed rectangle mask", "[adt][collision]") {
    std::vector<uint8_t> liquid(3072+24+24+1);
    put(liquid,0,3072);put(liquid,4,1);
    const uint16_t type=2, format=2;
    std::memcpy(liquid.data()+3072,&type,2);
    std::memcpy(liquid.data()+3074,&format,2);
    const float minHeight=37.0f;
    std::memcpy(liquid.data()+3076,&minHeight,4);
    liquid[3084]=3;liquid[3085]=4;liquid[3086]=2;liquid[3087]=1;
    put(liquid,3088,uint32_t(liquid.size()-1));liquid.back()=0x02;
    std::vector<uint8_t> bytes;chunk(bytes,0x4d48324f,liquid);
    const auto data=wowee::pipeline::ADTLoader::load(bytes);
    REQUIRE(data.collisionDataValid);
    REQUIRE(data.collisionLiquids[0].size()==1);
    const auto& layer=data.collisionLiquids[0][0];
    CHECK(layer.minX==3);CHECK(layer.minY==4);CHECK(layer.maxX==5);CHECK(layer.maxY==5);
    CHECK(layer.cellMask==2);
    REQUIRE(layer.heights.size()==6);
    CHECK(layer.heights.front()==0.0f); // Format 2's original GetHeight returns zero.
    CHECK(layer.heights.back()==0.0f);
}

TEST_CASE("ADT chunk coordinates own their collision references, independent of file order", "[adt][collision]") {
    std::vector<uint8_t> first(128),second(128),bytes;
    put(first,4,5);put(first,8,4);put(first,52,123);
    put(second,52,456);
    chunk(bytes,0x4d434e4b,first);chunk(bytes,0x4d434e4b,second);
    const auto data=wowee::pipeline::ADTLoader::load(bytes);
    REQUIRE(data.collisionDataValid);
    CHECK(data.getChunk(5,4).areaId==123);
    CHECK(data.getChunk(0,0).areaId==456);
}

TEST_CASE("MH2O rectangles outside the chunk cannot write a water mask", "[adt][collision]") {
    std::vector<uint8_t> liquid(3072+24);
    put(liquid,0,3072);put(liquid,4,1);
    liquid[3084]=9;liquid[3085]=0;liquid[3086]=1;liquid[3087]=1;
    std::vector<uint8_t> bytes;chunk(bytes,0x4d48324f,liquid);
    const auto data=wowee::pipeline::ADTLoader::load(bytes);
    CHECK_FALSE(data.collisionDataValid);
    CHECK(data.collisionLiquids[0].empty());
    CHECK(data.waterData[0].layers.empty());
}
