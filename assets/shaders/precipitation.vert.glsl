#version 450

// Rain, snow and sand, drawn from nothing but the vertex index: each six
// vertices are one drop, a quad, placed by a hash of its index in a box that
// follows the camera. The drops are fixed in the world - the box is a window
// onto an endless field of them, wrapped round as the camera moves - and fall
// by the distance the CPU has accumulated (Precipitation::Layer::drift), so a
// change of wind changes how fast they go from where they are.
//
// A drop below the highest surface over it (the shelter map) is under cover
// and is not drawn: no rain under a roof, a bridge or a tree. The splashes
// land on that same surface.
//
// Kinds (push.shape.x):
//   0 rain     streaks along the drop's velocity, a pixel wide or more
//   1 snow     flakes facing the camera, fluttering
//   2 grains   sand, streaks driven along the wind
//   3 sheets   sand, broad drifting veils of dust a little over the ground
//   4 splash   rain landing on the surface the shelter map has, or on water

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    vec4 fogSkyTop;  // rgb = zenith, w = sky tint amount (0 disables it)
    vec4 fogSkyMiddle;
    vec4 fogSkyBand1;
    vec4 fogSkyBand2;
    mat4 contactSpaceMatrix;  // read by contact_shadows.glsli
    vec4 contactParams;
    vec4 windDirLean;
    vec4 windPhases;
    vec4 windTravel;
    vec4 skySmogColor;
    vec4 aerialSun;
    vec4 aerialLight;
    vec4 terrainLodParams;
    vec4 aerialParams;
};

layout(set = 1, binding = 0) uniform sampler2D uShelter;

// The water's height over a square round the camera, a cell at a time
// (Precipitation::WaterGrid); -1e9 where there is none.
layout(std430, set = 1, binding = 1) readonly buffer WaterGrid {
    vec4 waterOrigin;  // xy the square's low corner, z a cell's size, w cells a side
    float waterHeights[];
};

layout(push_constant) uniform Push {
    vec4 cameraBox;     // xyz the camera, w the box's width (yards)
    vec4 driftHeight;   // xyz how far the layer has fallen and blown (yards), w the box's height
    vec4 velocity;      // xyz a drop's velocity (yd/s), w how long a streak is in seconds
    vec4 shape;         // x kind, y size (yards), z opacity, w time (s)
    vec4 shelter;       // xy the shelter map's centre, z its half extent, w its ceiling
    vec4 shelterRange;  // x yards from the ceiling to depth 1, y drawn
    vec4 tint;          // rgb what the drops are tinted, a the layer's seed
    vec4 screen;        // xy the viewport in pixels, z how much of the box is below the camera
} pc;

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out float vKind;
layout(location = 3) out float vAge;
layout(location = 4) flat out float vSeed;

const int KIND_RAIN = 0;
const int KIND_SNOW = 1;
const int KIND_GRAINS = 2;
const int KIND_SHEETS = 3;
const int KIND_SPLASH = 4;
/// What a splash on water becomes: a ring spreading on the surface.
const int KIND_RIPPLE = 5;

const vec2 kCorners[6] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(-1.0, 1.0),
                                vec2(-1.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 hash3(uint i) {
    uint a = pcg(i);
    uint b = pcg(a ^ 0x9e3779b9u);
    uint c = pcg(b ^ 0x85ebca6bu);
    return vec3(a, b, c) * (1.0 / 4294967295.0);
}

/// The highest surface over `xy`, if the shelter map has it.
bool surfaceAt(vec2 xy, out float top) {
    top = -1e9;
    if (pc.shelterRange.y < 0.5) return false;
    vec2 uv = (xy - pc.shelter.xy) / pc.shelter.z * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
    float depth = textureLod(uShelter, uv, 0.0).r;
    if (depth >= 1.0) return false;
    top = pc.shelter.w - depth * pc.shelterRange.x;
    return true;
}

/// The water's surface over `xy`, if there is water there.
bool waterAt(vec2 xy, out float height) {
    height = -1e9;
    if (waterOrigin.z <= 0.0) return false;
    int cells = int(waterOrigin.w);
    ivec2 cell = ivec2(floor((xy - waterOrigin.xy) / waterOrigin.z));
    if (any(lessThan(cell, ivec2(0))) || any(greaterThanEqual(cell, ivec2(cells)))) return false;
    height = waterHeights[cell.y * cells + cell.x];
    return height > -1e8;
}

bool underCover(vec3 p) {
    float top;
    return surfaceAt(p.xy, top) && p.z < top - 0.05;
}

void hide() {
    // Behind the far plane: the whole quad is clipped.
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
    vLocal = vec2(0.0);
    vColor = vec4(0.0);
    vKind = 0.0;
    vAge = 0.0;
    vSeed = 0.0;
}

/// The light a drop catches: the sky's, the air's and the sun's, glinting
/// most when the camera looks toward the sun through it.
vec3 dropLight(vec3 p, float glint) {
    vec3 toSun = normalize(-lightDir.xyz);
    vec3 look = normalize(p - viewPos.xyz);
    float forward = pow(max(dot(look, toSun), 0.0), 6.0);
    float sunUp = clamp(toSun.z * 2.0 + 0.2, 0.0, 1.0);
    return ambientColor.rgb * 0.9 + fogColor.rgb * 0.5 + lightColor.rgb * (0.15 * sunUp + glint * forward);
}

void main() {
    int kind = int(pc.shape.x + 0.5);
    uint drop = uint(gl_VertexIndex) / 6u;
    vec2 corner = kCorners[uint(gl_VertexIndex) % 6u];
    uint seed = uint(pc.tint.a);
    vec3 h = hash3(drop * 7u + seed * 131u);
    float time = pc.shape.w;
    vSeed = h.x;
    vAge = 0.0;

    vec3 camera = pc.cameraBox.xyz;
    mat4 viewProj = projection * view;

    if (kind == KIND_SPLASH) {
        // A drop landing: somewhere in a disc round the camera, again each
        // cycle, on whatever the shelter map says is highest there.
        const float kLife = 0.4;
        float cycles = time / kLife + h.y;
        uint cycle = uint(floor(cycles));
        float age = fract(cycles);
        vec3 where = hash3(drop * 7919u + cycle * 104729u + seed);
        float radius = pc.cameraBox.w * sqrt(where.x);
        float angle = where.y * 6.2831853;
        vec2 xy = camera.xy + radius * vec2(cos(angle), sin(angle));
        float top;
        if (!surfaceAt(xy, top) || abs(top - camera.z) > 40.0) {
            hide();
            return;
        }
        // Water over what the shelter map has there - a lake's bed, a canal's
        // floor: the drop rings on the surface instead.
        float waterHeight;
        bool onWater = waterAt(xy, waterHeight) && waterHeight > top - 0.05;
        vec3 base = vec3(xy, onWater ? waterHeight + 0.02 : top + 0.01);
        float size = pc.shape.y * (0.7 + 0.6 * where.z);
        vec3 worldPos;
        if (onWater) {
            // Flat on the surface.
            vec2 local = vec2(corner.x, corner.y * 2.0 - 1.0);
            worldPos = base + vec3(local * size * 1.4, 0.0);
            vLocal = local;
            vKind = float(KIND_RIPPLE);
        } else {
            // Standing up from it, turned to the camera.
            vec2 toCamera = camera.xy - xy;
            vec2 across = length(toCamera) > 1e-3 ? normalize(vec2(-toCamera.y, toCamera.x)) : vec2(1.0, 0.0);
            worldPos = base + vec3(across * corner.x * size, corner.y * size * 0.8);
            vLocal = corner;
            vKind = float(KIND_SPLASH);
        }
        vAge = age;
        float fade = 1.0 - smoothstep(0.6, 1.0, radius / pc.cameraBox.w);
        vColor = vec4(dropLight(base, 0.8) * pc.tint.rgb, pc.shape.z * fade);
        gl_Position = viewProj * vec4(worldPos, 1.0);
        return;
    }

    // The drop's place in the field, wrapped into the box round the camera.
    // Its speed is one of five tenths either side of the layer's, and the
    // CPU wraps the drift at ten boxes, so every speed wraps whole.
    float speed = 0.8 + 0.1 * float(pcg(drop ^ (seed * 2654435761u)) % 5u);
    vec3 box = vec3(pc.cameraBox.w, pc.cameraBox.w, pc.driftHeight.w);
    vec3 low = camera - vec3(box.xy * 0.5, box.z * pc.screen.z);
    vec3 p = h * box + pc.driftHeight.xyz * speed;
    p = low + mod(p - low, box);
    vec3 inBox = (p - low) / box;
    float edge = min(min(inBox.x, 1.0 - inBox.x), min(inBox.y, 1.0 - inBox.y));
    float fade = smoothstep(0.0, 0.12, edge) * smoothstep(0.0, 0.08, min(inBox.z, 1.0 - inBox.z));

    if (kind == KIND_SNOW) {
        // Flakes tumble as they fall.
        float rate = 0.6 + 0.8 * h.z;
        p.xy += vec2(sin(time * rate * 2.1 + h.x * 40.0), cos(time * rate * 1.7 + h.y * 40.0)) * 0.3;
        p.z += sin(time * rate * 3.3 + h.z * 40.0) * 0.08;
    }
    if (kind == KIND_SHEETS) {
        // Veils of dust a little over the ground, wherever the ground is.
        float ground;
        p.z = (surfaceAt(p.xy, ground) && abs(ground - camera.z) < 30.0 ? ground : camera.z - 1.6)
              + pc.shape.y * (0.25 + 0.3 * h.z);
        fade = smoothstep(0.0, 0.2, edge);
    } else if (underCover(p)) {
        hide();
        return;
    }

    float dist = length(p - viewPos.xyz);
    // Not in the camera's face: a drop a hand's breadth away is a smear.
    fade *= smoothstep(0.3, 1.0, dist);

    vec4 clip;
    float coverage = 1.0;
    if (kind == KIND_RAIN || kind == KIND_GRAINS) {
        vec3 velocity = pc.velocity.xyz * speed;
        vec3 head = p;
        vec3 tail = p - velocity * pc.velocity.w;
        vec4 ch = viewProj * vec4(head, 1.0);
        vec4 ct = viewProj * vec4(tail, 1.0);
        if (ch.w < 0.05 || ct.w < 0.05) {
            hide();
            return;
        }
        vec2 halfPx = pc.screen.xy * 0.5;
        vec2 along = (ch.xy / ch.w - ct.xy / ct.w) * halfPx;
        float len = length(along);
        vec2 dir = len > 1e-3 ? along / len : vec2(0.0, 1.0);
        vec2 across = vec2(-dir.y, dir.x);
        // As wide as the drop is at its distance, and never thinner than a
        // pixel and a half - a thinner quad misses most pixel centres and
        // breaks into dots: what is thinner is drawn fainter instead.
        // The projection turns y over for Vulkan; the scale is its size.
        float widthPx = pc.shape.y * abs(projection[1][1]) / ch.w * halfPx.y;
        float drawPx = max(widthPx, 1.5);
        // Fainter, but not in proportion: a streak far off is still seen.
        coverage = clamp(sqrt(widthPx / drawPx), 0.0, 1.0);
        clip = mix(ct, ch, corner.y);
        clip.xy += across * corner.x * drawPx / halfPx * clip.w;
        vLocal = corner;
    } else {
        // A quad facing the camera.
        vec3 right = vec3(view[0][0], view[1][0], view[2][0]);
        vec3 up = vec3(view[0][1], view[1][1], view[2][1]);
        float size = pc.shape.y * (0.6 + 0.8 * h.y);
        vec2 local = vec2(corner.x, corner.y * 2.0 - 1.0);
        clip = viewProj * vec4(p + (right * local.x + up * local.y) * size, 1.0);
        if (kind == KIND_SNOW) {
            float sizePx = size * abs(projection[1][1]) / max(clip.w, 0.05) * pc.screen.y * 0.5;
            coverage = clamp(sizePx / 1.5, 0.15, 1.0);
        }
        vLocal = local;
    }

    vec3 light;
    if (kind == KIND_RAIN) {
        light = dropLight(p, 1.5);
    } else if (kind == KIND_SNOW) {
        vec3 toSun = normalize(-lightDir.xyz);
        light = 0.9 * (ambientColor.rgb * 1.1 + fogColor.rgb * 0.3 +
                       lightColor.rgb * 0.6 * clamp(toSun.z * 2.0 + 0.2, 0.0, 1.0));
    } else {
        vec3 toSun = normalize(-lightDir.xyz);
        vec3 sand = vec3(0.78, 0.64, 0.44);
        light = sand * (ambientColor.rgb * 1.2 + fogColor.rgb * 0.4 +
                        lightColor.rgb * 0.6 * clamp(toSun.z * 2.0 + 0.2, 0.0, 1.0));
    }
    vColor = vec4(light * pc.tint.rgb, pc.shape.z * fade * coverage);
    vKind = float(kind);
    // The dust's own churning, for the veils.
    vAge = time * 0.05 + h.z * 10.0;
    gl_Position = clip;
}
