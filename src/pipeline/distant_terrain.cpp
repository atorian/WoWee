#include "pipeline/distant_terrain.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
namespace wowee::pipeline {
namespace {
uint32_t u32(std::span<const uint8_t> data, size_t at) {
    uint32_t v; std::memcpy(&v, data.data()+at, 4); return v;
}
bool tag(std::span<const uint8_t> b, size_t at, const char* normal, const char* reversed) {
    return at+8<=b.size() && (!std::memcmp(b.data()+at,normal,4) || !std::memcmp(b.data()+at,reversed,4));
}
}
DistantMap readDistantMap(std::span<const uint8_t> bytes) {
    DistantMap result;
    size_t offsets = 0;
    for (size_t p=0; p+8<=bytes.size();) {
        const size_t size=u32(bytes,p+4);
        if (size>bytes.size()-p-8) { LOG_ERROR("WDL: truncated chunk at ",p); return {}; }
        if (tag(bytes,p,"MAOF","FOAM")) {
            if (size!=4096*4) { LOG_ERROR("WDL: invalid MAOF size ",size); return {}; }
            offsets=p+8; break;
        }
        p+=size+8;
    }
    if (!offsets) { LOG_ERROR("WDL: no tile offsets (MAOF)"); return {}; }
    for (int y=0;y<64;++y) for(int x=0;x<64;++x) {
        size_t p=u32(bytes,offsets+4*(y*64+x));
        if (!p) continue;
        if (!tag(bytes,p,"MARE","ERAM") || u32(bytes,p+4)<545*2 || bytes.size()-p-8<545*2) {
            LOG_ERROR("WDL: invalid height tile [",x,",",y,"] at ",p); return {};
        }
        DistantTile tile; tile.x=x; tile.y=y;
        std::memcpy(tile.outer.data(),bytes.data()+p+8,289*2);
        std::memcpy(tile.inner.data(),bytes.data()+p+8+289*2,256*2);
        p+=8+u32(bytes,p+4);
        if (tag(bytes,p,"MAHO","OHAM")) {
            if (u32(bytes,p+4)<32 || bytes.size()-p-8<32) { LOG_ERROR("WDL: truncated hole mask"); return {}; }
            std::memcpy(tile.holes.data(),bytes.data()+p+8,32);
        }
        result.tiles.push_back(std::move(tile));
    }
    result.valid=true;
    return result;
}
DistantMesh distantTileMesh(const DistantTile& tile) {
    DistantMesh out;
    constexpr float size=core::coords::TILE_SIZE, step=size/16;
    const auto position=[&](float x,float y,float z) {
        return glm::vec3(core::coords::ZEROPOINT-(tile.y*size+y*step),
                         core::coords::ZEROPOINT-(tile.x*size+x*step),z);
    };
    out.min=glm::vec3(1e20f);out.max=glm::vec3(-1e20f);
    for (int y=0;y<17;++y) for(int x=0;x<17;++x) {
        const float h=tile.outer[y*17+x];
        const float dx=(tile.outer[y*17+std::min(x+1,16)]-tile.outer[y*17+std::max(x-1,0)]) /
                       ((std::min(x+1,16)-std::max(x-1,0))*step);
        const float dy=(tile.outer[std::min(y+1,16)*17+x]-tile.outer[std::max(y-1,0)*17+x]) /
                       ((std::min(y+1,16)-std::max(y-1,0))*step);
        auto p=position(x,y,h);out.min=glm::min(out.min,p);out.max=glm::max(out.max,p);
        out.vertices.push_back({p,glm::normalize(glm::vec3(dy,dx,1)),glm::vec3(.45f),{x/16.f,y/16.f}});
    }
    for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
        auto p=position(x+.5f,y+.5f,tile.inner[y*16+x]);
        out.min=glm::min(out.min,p);out.max=glm::max(out.max,p);
        out.vertices.push_back({p,out.vertices[y*17+x].normal,glm::vec3(.45f),{(x+.5f)/16,(y+.5f)/16}});
    }
    const bool holes=std::any_of(tile.holes.begin(),tile.holes.end(),[](auto h){return h!=0;});
    for(int level=0;level<4;++level) {
        auto& range=out.lods[level];range.first=static_cast<uint32_t>(out.indices.size());
        if (level && holes) continue; // coarse cells must not seal a cave/whole-tile hole
        int stride=level==0 ? 1 : 1<<(level-1);
        for(int y=0;y<16;y+=stride)for(int x=0;x<16;x+=stride) {
            if(tile.holes[y]&(1u<<x))continue;
            const uint32_t a=y*17+x,b=a+stride,c=(y+stride)*17+x,d=c+stride;
            if(level==0) {
                uint32_t m=289+y*16+x;
                out.indices.insert(out.indices.end(),{a,m,b,b,m,d,d,m,c,c,m,a});
            } else out.indices.insert(out.indices.end(),{a,c,b,b,c,d});
        }
        range.count=static_cast<uint32_t>(out.indices.size())-range.first;
        if(level) {
            // Bound the deviation against every authored outer/inner sample.
            for(size_t i=0;i<545;++i) {
                auto uv=out.vertices[i].uv*16.f;
                int x=std::min(16-stride,int(uv.x)/stride*stride),y=std::min(16-stride,int(uv.y)/stride*stride);
                float u=(uv.x-x)/stride,v=(uv.y-y)/stride;
                float h= u+v<=1 ? tile.outer[y*17+x]*(1-u-v)+tile.outer[y*17+x+stride]*u+tile.outer[(y+stride)*17+x]*v
                    : tile.outer[(y+stride)*17+x+stride]*(u+v-1)+tile.outer[y*17+x+stride]*(1-v)+tile.outer[(y+stride)*17+x]*(1-u);
                range.error=std::max(range.error,std::abs(h-out.vertices[i].position.z));
            }
        }
        // Stitch mismatched tile levels and the integer WDL/ADT seam. Each
        // level uses its own boundary samples, so no T-junction opens a crack.
        const float skirtDepth = step + range.error;
        for (int edge=0;edge<4;++edge) for(int n=0;n<16;n+=stride) {
            int x=edge==0 ? n : edge==1 ? n : edge==2 ? 0 : 16;
            int y=edge==0 ? 0 : edge==1 ? 16 : n;
            int nx=x+(edge<2 ? stride : 0), ny=y+(edge>=2 ? stride : 0);
            const int hx=std::min(x,15), hy=std::min(y,15);
            if(tile.holes[hy]&(1u<<hx))continue;
            const uint32_t a=y*17+x,b=ny*17+nx,c=static_cast<uint32_t>(out.vertices.size());
            auto va=out.vertices[a],vb=out.vertices[b];
            va.position.z-=skirtDepth;vb.position.z-=skirtDepth;
            out.vertices.push_back(va);out.vertices.push_back(vb);
            out.indices.insert(out.indices.end(),{a,c,b,b,c,c+1});
        }
        range.count=static_cast<uint32_t>(out.indices.size())-range.first;
    }
    return out;
}
}
