#include "collision/wmo_bsp_query.hpp"
#include <iostream>
#include <string>

int main() {
    std::string name;
    auto vector = [](glm::vec3& v) { return bool(std::cin >> v.x >> v.y >> v.z); };
    while (std::cin >> name) {
        size_t vertexCount, triangleCount, nodeCount, faceCount;
        uint32_t flags;
        bool cache;
        glm::vec3 boundsMin, boundsMax, queryMin, queryMax;
        if (!(std::cin >> vertexCount >> triangleCount >> nodeCount >> faceCount >> flags >> cache) ||
            vertexCount>65536 || triangleCount>65536 || nodeCount>65536 || faceCount>1000000 ||
            !vector(boundsMin) || !vector(boundsMax) || !vector(queryMin) || !vector(queryMax)) return 2;
        std::vector<glm::vec3> vertices(vertexCount);
        for (auto& v:vertices) if (!vector(v)) return 2;
        std::vector<uint16_t> indices(triangleCount*3), faces(faceCount);
        std::vector<uint8_t> mopy(triangleCount);
        for (auto& i:indices) if (!(std::cin >> i)) return 2;
        for (auto& f:mopy) { unsigned v; if (!(std::cin >> v) || v>255) return 2; f=uint8_t(v); }
        std::vector<wowee::pipeline::WmoBspNode> nodes(nodeCount);
        for (auto& n:nodes) {
            if (!(std::cin >> n.flags >> n.negativeChild >> n.positiveChild >> n.faceCount
                          >> n.firstFace >> n.planeDistance)) return 2;
        }
        for (auto& f:faces) if (!(std::cin >> f)) return 2;
        std::string error;
        auto bsp=wowee::pipeline::WmoBspData::build(std::move(nodes),std::move(faces),indices,vertices.size(),error);
        if (!bsp) { std::cerr << name << ": " << error << '\n'; return 3; }
        const auto result=wowee::collision::queryWmoBsp(*bsp,vertices,indices,mopy,
            boundsMin,boundsMax,queryMin,queryMax,flags,cache);
        std::cout << "{\"name\":\"" << name << "\",\"tested\":[";
        auto list=[](const auto& values) {
            for (size_t i=0;i<values.size();++i) { if(i) std::cout<<','; std::cout<<values[i]; }
        };
        list(result.tested);
        std::cout << "],\"faces\":["; list(result.faces);
        std::cout << "],\"overflow\":" << result.overflow << "}\n";
    }
    return std::cin.eof()?0:2;
}
