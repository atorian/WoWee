#version 450

// One step of the effective fetch over the sea bed's map (sea_bed.glsli),
// for one radial at a time, in a grid turned so its rows run up the radial:
//   mode 0  reads the shore into the grid from the map
//   mode 1  doubles how far up its row each point looks for one
//   mode 2  adds the radial's share of its fetch into the effective fetch,
//           which lies along the map's own axes
// Points whose step upwind leaves the grid have met no shore within it.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, r32f) uniform readonly image2D bed;
layout(set = 0, binding = 1, r32f) uniform readonly image2D fetchFrom;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D fetchTo;
// x = the shares of the radials' fetches that met a shore, and of the
// least the others can be; y = the share of those that met none.
layout(set = 0, binding = 3, rg32f) uniform image2D effective;

layout(push_constant) uniform Push {
    vec2 upwind;          // unit, in the map's x and y: toward where this radial comes from
    float step;           // texels mode 1 looks upwind
    float texelYards;     // of the grid
    float range;          // yards from the water to the views' foot
    float openFetch;      // what marks no shore met (kOceanOpenFetch)
    float share;          // this radial's share of the effective fetch
    float leastUnmet;     // the least fetch a radial that met no shore is taken for
    int mode;
    int first;            // mode 2: the first radial, which starts the sum
} push;

#include "sea_bed.glsli"

void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(fetchTo);
    ivec2 bedSize = imageSize(bed);
    if (push.mode == 0) {
        vec2 mapUv = seaFetchGridToMap((vec2(texel) + 0.5) / vec2(size), push.upwind);
        if (any(lessThan(mapUv, vec2(0.0))) || any(greaterThanEqual(mapUv, vec2(1.0)))) {
            imageStore(fetchTo, texel, vec4(push.openFetch));
            return;
        }
        // The map read across the texel, turned with it.
        int shore = 0;
        const int side = kSeaFetchShoreSamplesSide;
        for (int sy = 0; sy < side; ++sy) {
            for (int sx = 0; sx < side; ++sx) {
                vec2 within = (vec2(texel) + (vec2(sx, sy) + 0.5) / float(side)) / vec2(size);
                ivec2 at = ivec2(seaFetchGridToMap(within, push.upwind) * vec2(bedSize));
                float depth = imageLoad(bed, clamp(at, ivec2(0), bedSize - 1)).r;
                if (seaBedIsShore(depth, push.range)) ++shore;
            }
        }
        imageStore(fetchTo, texel,
                   vec4(seaBedFetchStart(seaFetchCellIsShore(shore, side * side), push.openFetch)));
    } else if (push.mode == 1) {
        float here = imageLoad(fetchFrom, texel).r;
        int from = texel.x + int(push.step);
        float upwind = from < size.x ? imageLoad(fetchFrom, ivec2(from, texel.y)).r : push.openFetch;
        imageStore(fetchTo, texel, vec4(seaBedFetchPass(here, upwind, push.step * push.texelYards)));
    } else {
        // Texel `texel` of the effective fetch, along the map's axes, from
        // the radial's grid.
        vec2 grid = seaFetchMapToGrid((vec2(texel) + 0.5) / vec2(size), push.upwind);
        ivec2 g = ivec2(floor(grid * vec2(size)));
        float fetch = push.openFetch;
        float toEdge = 0.0;
        if (all(greaterThanEqual(g, ivec2(0))) && all(lessThan(g, size))) {
            fetch = imageLoad(fetchFrom, g).r;
            toEdge = (float(size.x) - (float(g.x) + 0.5)) * push.texelYards;
        }
        vec2 sum = push.first != 0 ? vec2(0.0) : imageLoad(effective, texel).xy;
        imageStore(effective, texel,
                   vec4(sum + seaFetchRadialPart(fetch, toEdge, push.share, push.openFetch, push.leastUnmet), 0.0, 0.0));
    }
}
