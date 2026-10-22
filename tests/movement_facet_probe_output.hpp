#pragma once
#include "collision/movement_facet.hpp"
#include <iostream>
#include <vector>

namespace facet_probe {
inline void writeFacets(const std::vector<wowee::collision::MovementFacet>& facets) {
        for(size_t i=0;i<facets.size();++i){
            if(i)std::cout<<',';const auto& f=facets[i];
            std::cout<<"{\"owner\":"<<f.owner<<",\"plane\":["<<f.plane.normal.x<<','<<f.plane.normal.y<<','<<f.plane.normal.z<<','<<f.plane.offset<<"],\"vertices\":[";
            for(size_t j=0;j<3;++j){if(j)std::cout<<',';const auto& v=f.vertices[j];std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']';}
            std::cout<<"]}";
        }
}
} // namespace facet_probe
