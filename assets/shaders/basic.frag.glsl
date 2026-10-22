#version 450

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
    vec4 rtTraceParams;
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
    mat4 lightningMatrix;
    vec4 lightningPosition;
    vec4 lightningColor;
};

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform BasicMaterial {
    vec4 color;
    vec3 lightPos;
    int useTexture;
};

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 ambient = 0.3 * vec3(1.0);
    vec3 norm = normalize(Normal);
    vec3 lightDir2 = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir2), 0.0);
    vec3 diffuse = diff * vec3(1.0);

    vec3 viewDir2 = normalize(viewPos.xyz - FragPos);
    vec3 reflectDir = reflect(-lightDir2, norm);
    float spec = pow(max(dot(viewDir2, reflectDir), 0.0), 32.0);
    vec3 specular = 0.5 * spec * vec3(1.0);

    vec3 result = ambient + diffuse + specular;

    if (useTexture != 0) {
        outColor = texture(uTexture, TexCoord) * vec4(result, 1.0);
    } else {
        outColor = color * vec4(result, 1.0);
    }
}
