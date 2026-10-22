#include "collision/movement_facet.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::cout << std::setprecision(9) << std::showpoint;
    std::string name;
    while (std::cin >> name) {
        bool sse;
        uint64_t owner;
        size_t batches;
        if (!(std::cin >> sse >> owner >> batches) || batches > 32) return 2;
        std::vector<wowee::collision::MovementFacet> facets;
        size_t diagnostics = 0;
        for (size_t batch = 0; batch < batches; ++batch) {
            size_t vertexCount, triangleCount;
            if (!(std::cin >> vertexCount >> triangleCount) || vertexCount > 65536 || triangleCount > 65535) return 2;
            glm::mat4 matrix;
            for (int c=0;c<4;++c) for (int r=0;r<4;++r) if (!(std::cin >> matrix[c][r])) return 2;
            std::vector<glm::vec3> vertices(vertexCount);
            for (auto& v : vertices) if (!(std::cin >> v.x >> v.y >> v.z)) return 2;
            for (size_t i=0;i<triangleCount;++i) {
                size_t a,b,c;
                if (!(std::cin >> a >> b >> c) || a>=vertexCount || b>=vertexCount || c>=vertexCount) return 2;
                auto result = wowee::collision::makeMovementFacet({vertices[a],vertices[b],vertices[c]},matrix,owner,
                    sse ? wowee::collision::FacetNormalization::NativeSse : wowee::collision::FacetNormalization::X87);
                if (!result) { std::cerr << "Requested SSE path is unavailable\n"; return 3; }
                diagnostics += result->degenerate;
                facets.push_back(result->facet);
            }
        }
        std::cout << "{\"name\":\"" << name << "\",\"diagnostics\":" << diagnostics << ",\"facets\":[";
        auto vector = [](const glm::vec3& v) { std::cout << '[' << v.x << ',' << v.y << ',' << v.z << ']'; };
        for (size_t i=0;i<facets.size();++i) {
            if (i) std::cout << ',';
            const auto& f = facets[i];
            std::cout << "{\"owner\":" << f.owner << ",\"plane\":[" << f.plane.normal.x << ','
                      << f.plane.normal.y << ',' << f.plane.normal.z << ',' << f.plane.offset << "],\"vertices\":[";
            for (size_t j=0;j<3;++j) { if(j)std::cout<<','; vector(f.vertices[j]); }
            std::cout << "]}";
        }
        std::cout << "]}\n";
    }
    return std::cin.eof() ? 0 : 2;
}
