// Offline front door for original-code comparisons. No game, device or socket.
#include "collision/movement_surface.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const bool swimming = argc == 2 && std::string(argv[1]) == "--swim";
    const bool falling = argc == 2 && std::string(argv[1]) == "--fall";
    using namespace wowee::collision;
    std::cout << std::setprecision(9) << std::showpoint;
    auto read3 = [](glm::vec3& v) { return bool(std::cin >> v.x >> v.y >> v.z); };
    auto read2 = [](glm::vec2& v) { return bool(std::cin >> v.x >> v.y); };
    std::string name;
    while (std::cin >> name) {
        SurfaceMovementState state;
        SurfaceMovementShape shape{};
        glm::vec2 direction;
        float distance;
        uint32_t elapsed;
        size_t count;
        if (!(std::cin >> shape.radius >> shape.height >> shape.stepHeight >> shape.clientControlled) ||
            !read3(state.position) || !read2(direction) ||
            !(std::cin >> distance >> elapsed >> state.flags >> state.stepStartZ >> state.facing
                       >> state.pitch >> state.flags2 >> state.anchorElapsedMs >> state.anchorFacing
                       >> state.anchorPitch >> state.fallTimeMs >> state.fallStartZ
                       >> state.fallVelocity >> state.currentSpeed) ||
            !read3(state.anchorPosition) || !read3(state.direction) || !read2(state.direction2D) ||
            !(std::cin >> count) || count > 100000) return 2;
        std::vector<MovementFacet> facets(count);
        for (auto& facet : facets) {
            if (!read3(facet.plane.normal) || !(std::cin >> facet.plane.offset >> facet.owner)) return 2;
            for (auto& v : facet.vertices) if (!read3(v)) return 2;
        }
        uint32_t time=1000;
        if ((falling || swimming) && (!(std::cin>>time) || !read3(state.groundNormal) ||
            !(std::cin>>state.walkSpeed>>state.runSpeed>>state.runBackSpeed>>state.swimSpeed
                      >>state.swimBackSpeed>>state.flightSpeed>>state.flightBackSpeed))) return 2;
        glm::vec3 swimDirection{};
        std::vector<MovementFacet> liquids;
        if (swimming) {
            if (!read3(swimDirection) || !(std::cin >> count) || count > 100000) return 2;
            liquids.resize(count);
            for (auto& facet : liquids) {
                if (!read3(facet.plane.normal) || !(std::cin >> facet.plane.offset >> facet.owner)) return 2;
                for (auto& v : facet.vertices) if (!read3(v)) return 2;
            }
        }
        const auto result = swimming ? traceMovementSwim(facets, liquids, state, shape, swimDirection, distance, elapsed)
                          : falling ? traceMovementFall(facets, state, shape, direction, distance, time, elapsed)
                                    : traceMovementSurface(facets, state, shape, direction, distance, elapsed);
        std::cout << "{\"name\":\"" << name << '"';
        auto number = [](const char* key, auto value) { std::cout << ",\"" << key << "\":" << value; };
        auto vector = [](const char* key, const glm::vec3& v) {
            std::cout << ",\"" << key << "\":[" << v.x << ',' << v.y << ',' << v.z << ']';
        };
        vector("position", state.position);
        vector("anchor_position", state.anchorPosition);
        vector("move_direction", state.direction);
        std::cout << ",\"move_direction_2d\":[" << state.direction2D.x << ',' << state.direction2D.y << ']';
        number("flags", state.flags);
        number("step_start", state.stepStartZ);
        number("elapsed", result.elapsedMs);
        number("fall_time", state.fallTimeMs);
        number("fall_start", state.fallStartZ);
        number("fall_velocity", state.fallVelocity);
        number("current_speed", state.currentSpeed);
        number("anchor_elapsed", state.anchorElapsedMs);
        number("anchor_facing", state.anchorFacing);
        number("anchor_pitch", state.anchorPitch);
        std::cout << ",\"transport\":";
        if (result.transport) std::cout << *result.transport;
        else std::cout << "null";
        if (falling || swimming) {
            vector("ground_normal",state.groundNormal);
            std::cout << ",\"fall_reset\":";
            if (result.fallResetTime) std::cout << *result.fallResetTime;
            else std::cout << "null";
        }
        std::cout << "}\n";
    }
    return std::cin.eof() ? 0 : 2;
}
