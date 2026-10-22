// Offline front door for original-code comparisons. No game, device or socket.
#include "collision/movement_surface.hpp"
#include "collision/movement_query.hpp"
#include "collision/movement_dimensions.hpp"
#include "collision/movement_request.hpp"
#include "collision/movement_frame.hpp"
#include "collision/movement_execution.hpp"
#include "world_facet_fixture.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

struct SuppliedWorld : wowee::collision::MovementFacetSource {
    std::vector<wowee::collision::MovementFacet> solids,liquids;
    std::vector<std::pair<wowee::collision::MovementFacetRequest,uint32_t>> requests;
    size_t failQuery=0;
    wowee::collision::MovementFacetSource* collection=nullptr;
    wowee::collision::MovementGeometryStatus query(const wowee::collision::MovementFacetRequest& request,
        uint32_t flags,std::vector<wowee::collision::MovementFacet>& output) override {
        requests.emplace_back(request,flags);
        if (collection) return collection->query(request,flags,output);
        if (requests.size()==failQuery) { output.clear();return wowee::collision::MovementGeometryStatus::NotReady; }
        output=flags==0x20000?liquids:solids;
        return wowee::collision::MovementGeometryStatus::Ready;
    }
};

struct ExecutionObservations final:wowee::collision::MovementExecutionSink {
    wowee::collision::SurfaceMovementState& state;
    wowee::collision::SurfaceMovementShape& shape;
    struct Height {uint32_t time,counter;float value,applied;bool ack;uint32_t delay,flags2;};
    struct Heartbeat {uint32_t time;float height;glm::vec3 position;uint32_t flags;};
    std::vector<Height> heights;
    std::vector<Heartbeat> beats;
    std::vector<uint32_t> finished;
    ExecutionObservations(wowee::collision::SurfaceMovementState& s,wowee::collision::SurfaceMovementShape& h):state(s),shape(h){}
    void collisionHeightChanged(uint32_t time,float height,uint32_t counter,bool ack) override {
        heights.push_back({time,counter,height,shape.height,ack,state.moveStartDelayMs,state.flags2});
    }
    bool heartbeat(uint32_t time) override {
        beats.push_back({time,shape.height,state.position,state.flags});return true;
    }
    void frameFinished(uint32_t time) override {finished.push_back(time);}
    void print() const {
        std::cout<<",\"notifications\":[";
        for(size_t i=0;i<heights.size();++i) {
            if(i)std::cout<<',';
            const auto& e=heights[i];
            std::cout<<'['<<e.time<<",1303,"<<e.ack<<','<<e.value<<','<<e.counter<<",0,255,"<<e.applied<<','<<e.delay<<','<<e.flags2<<']';
        }
        std::cout<<"],\"heartbeats\":[";
        for(size_t i=0;i<beats.size();++i) {
            if(i)std::cout<<',';
            const auto& e=beats[i];
            std::cout<<'['<<e.time<<','<<e.height<<','<<e.position.x<<','<<e.position.y<<','<<e.position.z<<','<<e.flags<<']';
        }
        std::cout<<"],\"finished\":[";
        for(size_t i=0;i<finished.size();++i){if(i)std::cout<<',';std::cout<<finished[i];}
        std::cout<<']';
    }
};

int main(int argc, char** argv) {
    const bool execute = argc == 2 && std::string(argv[1]) == "--world-execute";
    const bool apply = execute || argc == 2 && std::string(argv[1]) == "--world-apply";
    const bool fullWorld = apply || argc == 2 && std::string(argv[1]) == "--world-request";
    const bool frame = fullWorld || (argc == 2 && std::string(argv[1]) == "--request");
    const bool dimensions = argc == 2 && std::string(argv[1]) == "--dimensions";
    const bool query = dimensions || (argc == 2 && std::string(argv[1]) == "--query");
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
            if (refresh && (!read3(request.queryMin) || !read3(request.queryMax))) return 2;
            if (dimensions) {
                size_t events;if(!(std::cin>>events)||events>100)return 2;
                for(size_t i=0;i<events;++i) {
                    unsigned kind;if(!(std::cin>>kind)||kind>1)return 2;
                    if(kind) {float height;if(!(std::cin>>height))return 2;setMovementCollisionHeight(shape,height);}
                    else {
                        float width,height,scale,reference;bool force;
                        if(!(std::cin>>width>>height>>scale>>reference>>force))return 2;
                        setMovementModelDimensions(shape,width,height,scale,reference,force);
                    }
                }
            }
            if (refresh) {
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
        ExecutionObservations observations(state,shape);
        MovementClock clock;bool retire=false;
        SuppliedWorld world;
        std::unique_ptr<world_fixture::Scene> scene;
        if (frame) {
            bool local,ghost;
            if (!(std::cin>>local>>ghost>>world.failQuery)) return 2;
            world.solids=facets;world.liquids=liquids;
            if(fullWorld) {
                world_fixture::Settings settings;if(!settings.read(std::cin))return 2;
                scene=std::make_unique<world_fixture::Scene>(settings);world.collection=scene->source.get();
            }
            MovementRequestCache cache;
            if(apply) {
                bool active;
                if(!(std::cin>>active>>state.moveStartDelayMs>>state.turnRate>>state.pitchRate>>state.pitchCos>>state.pitchSin))return 2;
                if(execute) {
                    uint32_t lastTime,count;bool defer;
                    if(!(std::cin>>lastTime>>clock.heartbeatTimeMs>>clock.idleTimeMs>>defer>>count))return 2;
                    MovementCommandQueue commands;
                    for(uint32_t i=0;i<count;++i) {
                        uint32_t eventTime,counter;float height;bool ack;
                        if(!(std::cin>>eventTime>>height>>counter>>ack))return 2;
                        commands.enqueueCollisionHeight(eventTime,height,counter,ack);
                    }
                    auto execution=executeUninterpolatedMovement(world,cache,state,shape,commands,clock,observations,time,lastTime,active,local,ghost,defer);
                    frameResult=std::move(execution.movement);retire=execution.retireRequested;
                } else frameResult=applyUninterpolatedMovement(world,cache,state,shape,time,elapsed,active,local,ghost);
            } else frameResult=traceUnattachedMovementRequest(world,cache,state,shape,swimDirection,time,elapsed,local,ghost);
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
        if(apply) {number("pitch_cos",state.pitchCos);number("pitch_sin",state.pitchSin);number("facing",state.facing);number("pitch",state.pitch);number("move_start_delay",state.moveStartDelayMs);}
        if(execute) {
            observations.print();number("height",shape.height);number("flags2",state.flags2);
            number("heartbeat_time",clock.heartbeatTimeMs);number("idle_time",clock.idleTimeMs);
            number("retire",retire?"true":"false");
        }
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
