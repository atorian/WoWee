#include "collision/movement_contact.hpp"
#include "collision/detail/movement_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace wowee::collision {
namespace {

// Recovered from build 12340. See tools/original_collision_reference.py for
// execution of the original machine code independently of this implementation.
constexpr float skin = 0.0013888889f;
constexpr float probeLength = 0.027777778f;
constexpr float tiny = 0.00000095367432f;
constexpr float divideThreshold = 0.00000023841858f;

double dot(const glm::vec3& a, const glm::vec3& b) {
    return double(a.z) * b.z + double(a.y) * b.y + double(a.x) * b.x;
}
double evaluate(const MovementPlane& plane, const glm::vec3& point) {
    return double(point.z) * plane.normal.z + double(point.x) * plane.normal.x +
           double(point.y) * plane.normal.y + plane.offset;
}

struct BodyVolume {
    std::array<MovementPlane, 9> planes;
    std::array<glm::vec3, 9> vertices;
};

// 0x75c8f0 and 0x75ca80. Four upright sides, top, four lower faces
// meeting at the feet. In particular, this is not a capsule or a cylinder.
BodyVolume volumeOf(const MovementBody& body) {
    const auto p = body.feet;
    const float r = body.radius;
    const float h = body.height;
    constexpr float a = 0.87964189f, b = 0.4756366f;
    BodyVolume result;
    result.planes = {{
        {{-1, 0, 0}, p.x-r}, {{1, 0, 0}, -p.x-r},
        {{0, 1, 0}, -p.y-r}, {{0, -1, 0}, p.y-r},
        {{0, 0, 1}, -p.z-h},
        {{-a, 0, -b}, float(double(p.z)*b + double(p.x)*a)},
        {{ a, 0, -b}, float(double(p.z)*b - double(p.x)*a)},
        {{0,  a, -b}, float(double(p.z)*b - double(p.y)*a)},
        {{0, -a, -b}, float(double(p.z)*b + double(p.y)*a)}
    }};
    const double lower = double(r) * 1.849399f;
    const float shoulder = float(lower + p.z);
    result.vertices = {{p,
        {p.x-r,p.y-r,shoulder}, {p.x-r,p.y+r,shoulder},
        {p.x+r,p.y+r,shoulder}, {p.x+r,p.y-r,shoulder},
        {p.x-r,p.y-r,p.z+h}, {p.x-r,p.y+r,p.z+h},
        {p.x+r,p.y+r,p.z+h}, {p.x+r,p.y-r,p.z+h}}};
    return result;
}

// Boundary point flags are not consumed by DistanceToMove. The retained
// vertices and their order match ClipPolygonToPlane at 0x75b710.
using Polygon = std::array<glm::vec3, 15>;
void clip(Polygon& polygon, size_t& count, const MovementPlane& plane) {
    std::array<float, 15> distances{};
    double minimum = std::numeric_limits<float>::max();
    double maximum = -minimum;
    for (size_t i=0; i<count; ++i) {
        const auto p=polygon[i];
        const double d = -(double(p.x)*plane.normal.x + double(p.z)*plane.normal.z +
                                double(p.y)*plane.normal.y + plane.offset);
        distances[i]=float(d);
        minimum=std::min(minimum,d);
        maximum=std::max(maximum,d);
    }
    if (minimum > -skin) return;
    if (maximum < skin) { count=0; return; }
    const auto source=polygon;
    const size_t oldCount=count;
    count=0;
    for (size_t i=0, previous=oldCount-1; i<oldCount; previous=i++) {
        const float currentD=distances[i], previousD=distances[previous];
        const bool currentInside=currentD>=0, previousInside=previousD>=0;
        if (previousInside != currentInside &&
            (previousInside ? previousD : currentD) > skin) {
            const double t=double(previousD)/(double(currentD)-previousD);
            glm::vec3 point;
            for (int axis=0;axis<3;++axis)
                point[axis]=float(double(source[previous][axis])-
                    (double(source[i][axis])-source[previous][axis])*t);
            polygon[count++]=point;
        }
        if (currentInside) polygon[count++]=source[i];
    }
    if (count<3) count=0;
}

// 0x75c0b0: retain shallow initial overlaps, but reject facets fully behind
// the body. This distinction is needed when leaving a contact or an edge.
bool polygonDistance(Polygon polygon, size_t count, const BodyVolume& volume,
                     size_t face, const glm::vec3& direction, float& distance) {
    const auto& plane=volume.planes[face];
    const double denominator=dot(direction,plane.normal);
    auto travel = [&](const glm::vec3& p) {
        const double value=evaluate(plane,p);
        return std::abs(denominator)>=divideThreshold ? value/denominator : value;
    };
    double best=distance;
    bool behind=true;
    for (size_t i=0;i<count;++i) {
        const double t=travel(polygon[i]);
        if (t<best) best=std::max(0.0,t);
        if (t>-tiny) behind=false;
    }
    if (behind) {
        for (size_t i=0;i<volume.planes.size();++i) {
            if (i!=face) clip(polygon,count,volume.planes[i]);
            if (!count) return false;
        }
        for (size_t i=0;i<count;++i)
            if (travel(polygon[i])>-probeLength) behind=false;
        if (behind) return false;
    }
    if (best>=distance) return false;
    distance=float(best);
    return true;
}

// 0x75c5a0: ordered facets, signed facing test, then clip against the
// extruded sides and solve travel against this body face.
bool traceFace(std::span<const MovementFacet> facets, const BodyVolume& volume,
               size_t face, std::span<const MovementPlane> sides,
               const glm::vec3& direction, float& distance, uint32_t& facetIndex) {
    bool hit=false;
    for (uint32_t i=0;i<facets.size();++i) {
        const auto& facet=facets[i];
        if (dot(facet.plane.normal,direction)>-0.0000099999997f) continue;
        Polygon polygon{};
        std::copy(facet.vertices.begin(),facet.vertices.end(),polygon.begin());
        size_t count=3;
        for (const auto& side:sides) {
            clip(polygon,count,side);
            if (!count) break;
        }
        if (!count) continue;
        float candidate=std::numeric_limits<float>::max();
        if (polygonDistance(polygon,count,volume,face,direction,candidate) && candidate<=distance) {
            distance=candidate;
            facetIndex=i;
            hit=true;
        }
    }
    return hit;
}

} // namespace

namespace detail {
// Side planes of the swept triangle/quad: 0x75bc50 and 0x75be80.
// The original forms the displaced point then subtracts the base. Preserve
// that operation (and its precision) instead of substituting a different sweep.
bool extrudeMovementFace(std::span<const glm::vec3> vertices, std::span<const uint8_t> face,
               const glm::vec3& move, std::span<MovementPlane> planes) {
    for (size_t i=0;i<face.size();++i) {
        const auto p=vertices[face[i]];
        const auto q=vertices[face[(i+1)%face.size()]];
        const auto previous=vertices[face[(i+face.size()-1)%face.size()]];
        const double ax=(double(p.x)+move.x)-p.x;
        const double ay=(double(p.y)+move.y)-p.y;
        const double az=(double(p.z)+move.z)-p.z;
        const double bx=double(q.x)-p.x, by=double(q.y)-p.y, bz=double(q.z)-p.z;
        // x87 spills bx, by, bz and ay before reusing them for the latter
        // two components (0x75bcf8..0x75bd20 / 0x75bf25..0x75bf4b).
        const double nx=by*az-ay*bz;
        const double ny=double(float(bz))*ax-az*float(bx);
        const double nz=double(float(bx))*float(ay)-ax*float(by);
        const double lengthSquared=nz*nz+ny*ny+nx*nx;
        if (lengthSquared<tiny) return false;
        const double inverse=1.0/std::sqrt(lengthSquared);
        auto& plane=planes[i];
        plane.normal={float(nx*inverse),float(ny*inverse),float(nz*inverse)};
        const double ux=nx*inverse, uy=ny*inverse, uz=nz*inverse;
        plane.offset=float(-(uz*p.z + uy*p.y + ux*p.x));
        const double side=uy*(double(previous.y)-p.y)+
                          (double(previous.z)-p.z)*uz+
                          (double(previous.x)-p.x)*ux;
        if (side>0) {plane.normal=-plane.normal;plane.offset=-plane.offset;}
    }
    return true;
}

} // namespace detail

MovementContact traceMovementBody(std::span<const MovementFacet> facets,
                                  const MovementBody& body,
                                  const glm::vec3& direction, float distance) {
    MovementContact result;
    result.facet=static_cast<uint32_t>(facets.size());
    if (std::abs(distance)<tiny) return result;
    const auto volume=volumeOf(body);
    const glm::vec3 movement=direction*std::max(distance,probeLength);
    result.distance=distance;
    // The lower pyramid is tested before the four sides and the top.
    constexpr std::array<std::array<uint8_t,4>,9> faces{{
        {{0,1,2,0}},{{0,3,4,0}},{{0,2,3,0}},{{0,4,1,0}},
        {{1,2,6,5}},{{3,4,8,7}},{{2,3,7,6}},{{4,1,5,8}},{{5,6,7,8}}
    }};
    for (size_t pass=0;pass<faces.size();++pass) {
        const size_t face=pass<4 ? pass+5 : pass-4;
        const auto& plane=volume.planes[face];
        if (dot(plane.normal,movement)<=0) continue;
        const size_t count=pass<4 ? 3 : 4;
        std::array<MovementPlane,4> sides;
        if (!detail::extrudeMovementFace(volume.vertices,std::span(faces[pass]).first(count),movement,sides)) continue;
        float allowed=distance;
        if (!traceFace(facets,volume,face,std::span(sides).first(count),direction,allowed,result.facet)) continue;
        if (allowed >= result.distance-skin) {
            if (result.distance+skin > allowed) result.planes[result.planeCount++]=plane;
        } else {
            result.planes[0]=plane;
            result.planeCount=1;
        }
        if (allowed<result.distance) {
            result.distance=allowed;
            if (result.planeCount>1) std::swap(result.planes[0],result.planes[result.planeCount-1]);
        }
    }
    if (result.distance<skin) result.distance=0;
    return result;
}

} // namespace wowee::collision
