#include "collision/m2_facets.hpp"
#include <iostream>
#include <iomanip>
#include <string>

int main() {
    using namespace wowee::collision;
    std::cout << std::setprecision(9) << std::showpoint;
    auto read3 = [](glm::vec3& v) { return bool(std::cin >> v.x >> v.y >> v.z); };
    std::string name;
    while (std::cin >> name) {
        size_t nv, nf;
        uint64_t owner;
        glm::vec3 low, high;
        glm::mat4 matrix;
        if (!(std::cin >> nv >> nf >> owner) || nv > 65536 || nf > 65535 || !read3(low) || !read3(high)) return 2;
        for (int c=0;c<4;++c) for(int r=0;r<4;++r) if (!(std::cin >> matrix[c][r])) return 2;
        std::vector<glm::vec3> vertices(nv), normals(nf);
        std::vector<uint16_t> indices(nf*3);
        for (auto& v : vertices) if (!read3(v)) return 2;
        for (auto& v : normals) if (!read3(v)) return 2;
        for (auto& i : indices) if (!(std::cin >> i) || i>=nv) return 2;
        const auto facets = queryM2Facets(vertices, indices, normals, matrix, low, high, owner);
        std::cout << "{\"name\":\"" << name << "\",\"facets\":[";
        for (size_t i=0;i<facets.size();++i) {
            if(i)std::cout << ',';
            const auto& f=facets[i];
            std::cout << "{\"owner\":" << f.owner << ",\"plane\":[" << f.plane.normal.x << ',' << f.plane.normal.y << ',' << f.plane.normal.z << ',' << f.plane.offset << "],\"vertices\":[";
            for(size_t j=0;j<3;++j){if(j)std::cout<<',';const auto v=f.vertices[j];std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']';}
            std::cout << "]}";
        }
        std::cout << "]}\n";
    }
    return std::cin.eof() ? 0 : 2;
}
