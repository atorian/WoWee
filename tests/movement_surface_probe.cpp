// Offline front door for original-code comparisons. No game, device or socket.
#include "collision/movement_surface.hpp"
#include "collision/movement_query.hpp"
#include "collision/movement_request.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

struct SuppliedWorld : wowee::collision::MovementFacetSource {
    std::vector<wowee::collision::MovementFacet> solids,liquids;
    std::vector<std::pair<wowee::collision::MovementFacetRequest,uint32_t>> requests;
    size_t failQuery=0;
    wowee::collision::MovementGeometryStatus query(const wowee::collision::MovementFacetRequest& request,
        uint32_t flags,std::vector<wowee::collision::MovementFacet>& output) override {
        requests.emplace_back(request,flags);
        if (requests.size()==failQuery) { output.clear();return wowee::collision::MovementGeometryStatus::NotReady; }
        output=flags==0x20000?liquids:solids;
        return wowee::collision::MovementGeometryStatus::Ready;
    }
};

int main(int argc, char** argv) {
    const bool frame = argc == 2 && std::string(argv[1]) == "--request";
    const bool query = argc == 2 && std::string(argv[1]) == "--query";
    const bool ground = argc == 2 && std::string(argv[1]) == "--ground";
    const bool hovering = argc == 2 && std::string(argv[1]) == "--hover";
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
        if ((falling || swimming || hovering || ground || query || frame) && (!(std::cin>>time) || !read3(state.groundNormal) ||
            !(std::cin>>state.walkSpeed>>state.runSpeed>>state.runBackSpeed>>state.swimSpeed
                      >>state.swimBackSpeed>>state.flightSpeed>>state.flightBackSpeed))) return 2;
        glm::vec3 swimDirection{};
        std::vector<MovementFacet> liquids;
        if (swimming || hovering || query || frame) {
            if (!read3(swimDirection) || !(std::cin >> count) || count > 100000) return 2;
            liquids.resize(count);
            for (auto& facet : liquids) {
                if (!read3(facet.plane.normal) || !(std::cin >> facet.plane.offset >> facet.owner)) return 2;
                for (auto& v : facet.vertices) if (!read3(v)) return 2;
            }
        }
        if (query) {
            bool local,ghost,refresh;
            if (!(std::cin>>local>>ghost>>refresh)) return 2;
            bool emit=true;
            MovementFacetRequest request;
            if (refresh) {
                if (!read3(request.queryMin) || !read3(request.queryMax)) return 2;
                auto expanded=expandMovementFacetRequest(request,state,shape,swimDirection,local,ghost);
                emit=expanded.has_value();
                if (expanded) request=*expanded;
            } else request=makeMovementFacetRequest(state,shape,swimDirection,distance,elapsed,local,ghost);
            auto box=[](const glm::vec3& a,const glm::vec3& b) {
                std::cout<<'['<<a.x<<','<<a.y<<','<<a.z<<','<<b.x<<','<<b.y<<','<<b.z<<']';
            };
            std::cout<<"{\"name\":\""<<name<<"\",\"requests\":[";
            for (int i=0;emit && i<(request.queryLiquid?2:1);++i) {
                if (i) std::cout<<',';
                std::cout<<"{\"body\":";box(request.bodyMin,request.bodyMax);
                std::cout<<",\"box\":";box(request.queryMin,request.queryMax);
                std::cout<<",\"flags\":"<<(i?0x20000:request.solidFlags)<<'}';
            }
            std::cout<<']';
            if (refresh) { std::cout<<",\"cache\":";box(request.queryMin,request.queryMax); }
            std::cout<<"}\n";
            continue;
        }
        if ((hovering || frame) && !(std::cin >> state.hoverHeight)) return 2;
        MovementRequestResult frameResult;
        SuppliedWorld world;
        if (frame) {
            bool local,ghost;
            if (!(std::cin>>local>>ghost>>world.failQuery)) return 2;
            world.solids=facets;world.liquids=liquids;
            MovementRequestCache cache;
            frameResult=traceUnattachedMovementRequest(world,cache,state,shape,swimDirection,time,elapsed,local,ghost);
        }
        if (ground) state.groundNormal = queryMovementGroundNormal(facets, MovementBody(state.position, shape.radius, shape.height));
        const auto result = frame ? SurfaceMovementResult{frameResult.elapsedMs,frameResult.transportRequest,frameResult.fallResetTime}
                          : ground ? SurfaceMovementResult{0, std::nullopt, std::nullopt}
                          : hovering ? traceMovementHover(facets, state, shape, swimDirection, distance, time, elapsed)
                          : swimming ? traceMovementSwim(facets, liquids, state, shape, swimDirection, distance, elapsed)
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
        if (falling || swimming || hovering || ground || query || frame) {
            vector("ground_normal",state.groundNormal);
            std::cout << ",\"fall_reset\":";
            if (result.fallResetTime) std::cout << *result.fallResetTime;
            else std::cout << "null";
        }
        if (frame) {
            number("geometry_status",int(frameResult.geometryStatus));
            std::cout<<",\"events\":[";
            for (size_t i=0;i<frameResult.events.size();++i) {
                const auto& e=frameResult.events[i];if (i) std::cout<<',';
                std::cout<<"{\"kind\":"<<int(e.kind)<<",\"time\":"<<e.time<<",\"remaining\":"<<e.remaining
                         <<",\"oldFlags\":"<<e.oldFlags<<",\"oldFlags2\":"<<e.oldFlags2
                         <<",\"wasJumping\":"<<(e.wasJumping?"true":"false")<<'}';
            }
            std::cout<<"],\"requests\":[";
            for (size_t i=0;i<world.requests.size();++i) {
                if (i) std::cout<<',';
                const auto& [r,flags]=world.requests[i];
                std::cout<<"{\"flags\":"<<flags<<",\"body\":["<<r.bodyMin.x<<','<<r.bodyMin.y<<','<<r.bodyMin.z
                         <<','<<r.bodyMax.x<<','<<r.bodyMax.y<<','<<r.bodyMax.z<<"],\"box\":["
                         <<r.queryMin.x<<','<<r.queryMin.y<<','<<r.queryMin.z<<','
                         <<r.queryMax.x<<','<<r.queryMax.y<<','<<r.queryMax.z<<"]}";
            }
            std::cout<<']';
        }
        std::cout << "}\n";
    }
    return std::cin.eof() ? 0 : 2;
}
