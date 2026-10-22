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
