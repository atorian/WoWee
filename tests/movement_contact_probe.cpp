// Offline protocol for comparing the production contact primitive with the
// original machine-code reference. See tools/check_original_collision.py.
#include "collision/movement_contact.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace wowee::collision;
    std::cout << std::setprecision(9);
    std::string name;
    auto vector = [](glm::vec3& v) { return bool(std::cin >> v.x >> v.y >> v.z); };
    while (std::cin >> name) {
        MovementBody body({0, 0, 0}, 0, 0);
        glm::vec3 direction;
        float distance;
        size_t count;
        if (!(std::cin >> body.radius >> body.height) || !vector(body.feet) ||
            !vector(direction) || !(std::cin >> distance >> count) || count>100000) return 2;
        std::vector<MovementFacet> facets(count);
        for (auto& facet:facets) {
            if (!vector(facet.plane.normal) || !(std::cin >> facet.plane.offset)) return 2;
            for (auto& vertex:facet.vertices) if (!vector(vertex)) return 2;
        }
        auto result=traceMovementBody(facets,body,direction,distance);
        std::cout << "{\"name\":\"" << name << "\",\"distance\":" << result.distance
                  << ",\"facet\":" << result.facet << ",\"planes\":[";
        for (uint32_t i=0;i<result.planeCount;++i) {
            const auto& p=result.planes[i];
            if (i) std::cout << ',';
            std::cout << '[' << p.normal.x << ',' << p.normal.y << ',' << p.normal.z << ',' << p.offset << ']';
        }
        std::cout << "]}\n";
    }
    return std::cin.eof()?0:2;
}
