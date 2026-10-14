// What an M2 says about how it is drawn: header flags, textures, materials,
// and every skin batch with the shading the client derives for it.
//
//   m2_dump <model.m2> [skin.skin]
//
// The skin defaults to <model>00.skin. Each batch line names its stages'
// textures, the blend mode and material flags, the combiner and coordinate
// sources m2BatchShading picks, the colour and transparency it starts at, and
// whether its stages carry UV animation - the questions a rendering rule that
// guesses from a file name should have asked the file instead.

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_combiner.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace wowee;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const char* combinerName(rendering::M2Combiner c) {
    static const char* const names[] = {
        "Opaque", "Mod", "Decal", "Add", "Mod2x", "Fade", "Opaque_Opaque", "Opaque_Mod",
        "Opaque_Add", "Opaque_Mod2x", "Opaque_Mod2xNA", "Opaque_AddNA", "Mod_Opaque", "Mod_Mod",
        "Mod_Add", "Mod_Mod2x", "Mod_Mod2xNA", "Mod_AddNA", "Add_Mod", "Mod2x_Mod2x",
        "Opaque_Mod2xNA_Alpha", "Opaque_AddAlpha", "Opaque_AddAlpha_Alpha"};
    const auto i = static_cast<size_t>(c);
    return i < std::size(names) ? names[i] : "?";
}

const char* coordsName(rendering::M2StageCoords c) {
    switch (c) {
        case rendering::M2StageCoords::Uv0: return "uv0";
        case rendering::M2StageCoords::Uv1: return "uv1";
        case rendering::M2StageCoords::Environment: return "env";
        case rendering::M2StageCoords::Uv0Still: return "uv0still";
    }
    return "?";
}

std::string textureAt(const pipeline::M2Model& m, size_t lookupIndex) {
    if (lookupIndex >= m.textureLookup.size()) return "<no lookup>";
    const uint16_t slot = m.textureLookup[lookupIndex];
    if (slot >= m.textures.size()) return "<bad slot>";
    const auto& t = m.textures[slot];
    if (t.type != 0) return "<type " + std::to_string(t.type) + ">";
    const auto cut = t.filename.find_last_of("\\/");
    return cut == std::string::npos ? t.filename : t.filename.substr(cut + 1);
}

bool hasUvAnimation(const pipeline::M2Model& m, uint16_t animIndex, uint32_t stage) {
    if (animIndex == 0xFFFF) return false;
    const size_t i = size_t{animIndex} + stage;
    return i < m.textureTransformLookup.size() && m.textureTransformLookup[i] != 0xFFFF &&
           m.textureTransformLookup[i] < m.textureTransforms.size();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: m2_dump <model.m2> [skin.skin]\n");
        return 2;
    }
    const std::string path = argv[1];
    const auto data = readFile(path);
    if (data.empty()) {
        std::fprintf(stderr, "m2_dump: cannot read '%s'\n", path.c_str());
        return 1;
    }
    auto model = pipeline::M2Loader::load(data);
    std::string skinPath = argc > 2 ? argv[2] : path.substr(0, path.size() - 3) + "00.skin";
    const auto skin = readFile(skinPath);
    if (skin.empty() || !pipeline::M2Loader::loadSkin(skin, model)) {
        std::fprintf(stderr, "m2_dump: cannot load skin '%s'\n", skinPath.c_str());
        return 1;
    }

    std::printf("%s  version %u  flags 0x%x  combos %zu  bones %zu  emitters %zu  ribbons %zu\n",
                model.name.c_str(), model.version, model.globalFlags,
                model.textureCombinerCombos.size(), model.bones.size(),
                model.particleEmitters.size(), model.ribbonEmitters.size());
    std::printf("textures:\n");
    for (size_t i = 0; i < model.textures.size(); ++i) {
        const auto& t = model.textures[i];
        std::printf("  [%zu] type %u flags 0x%x %s\n", i, t.type, t.flags, t.filename.c_str());
    }
    std::printf("materials:\n");
    for (size_t i = 0; i < model.materials.size(); ++i)
        std::printf("  [%zu] blend %u flags 0x%x\n", i, model.materials[i].blendMode,
                    model.materials[i].flags);
    std::printf("batches:\n");
    for (size_t i = 0; i < model.batches.size(); ++i) {
        const auto& b = model.batches[i];
        const uint16_t blend = b.materialIndex < model.materials.size()
            ? model.materials[b.materialIndex].blendMode : 0;
        const uint16_t mflags = b.materialIndex < model.materials.size()
            ? model.materials[b.materialIndex].flags : 0;
        const auto coordAt = [&](size_t k) -> int16_t {
            return k < model.textureCoordLookup.size() ? model.textureCoordLookup[k] : 0;
        };
        const auto s = rendering::m2BatchShading(model.globalFlags, model.textureCombinerCombos,
                                                 b.shader, b.textureCount, blend,
                                                 coordAt(b.textureUnit), coordAt(b.textureUnit + 1u));
        float alpha = 1.0f, weight = 1.0f;
        if (b.colorIndex < model.colorAlphas.size()) alpha = model.colorAlphas[b.colorIndex];
        if (b.transparencyIndex < model.textureWeightLookup.size()) {
            const uint16_t w = model.textureWeightLookup[b.transparencyIndex];
            if (w < model.textureWeights.size()) weight = model.textureWeights[w];
        }
        std::string rgb = "-";
        if (b.colorIndex < model.colorRGB.size()) {
            const auto c = model.colorRGB[b.colorIndex];
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.2f,%.2f,%.2f", c.r, c.g, c.b);
            rgb = buf;
        }
        std::printf("  [%zu] geoset %u tris %u shader 0x%x blend %u mflags 0x%x prio %d -> %s%s"
                    " stages %u (%s%s, %s%s)%s\n"
                    "       t0 %s%s  t1 %s  colour %s a %.2f weight %.2f\n",
                    i, b.submeshId, b.indexCount / 3, b.shader, blend, mflags, b.priorityPlane,
                    combinerName(s.combiner), s.drawn ? "" : " (not drawn)", s.stages,
                    coordsName(s.coords0), hasUvAnimation(model, b.textureAnimIndex, 0) ? "+anim" : "",
                    coordsName(s.coords1), hasUvAnimation(model, b.textureAnimIndex, 1) ? "+anim" : "",
                    rendering::m2CombinerUsesSecondStage(s.combiner) ? "" : " [t1 unused]",
                    textureAt(model, b.textureIndex).c_str(),
                    b.textureCount > 1 ? "" : " (1 tex)",
                    b.textureCount > 1 ? textureAt(model, size_t{b.textureIndex} + 1).c_str() : "-",
                    rgb.c_str(), alpha, weight);
    }
    // A track as its first key and how it moves: "0.80" for a constant,
    // "0.80~3k gs1" for three keys on global sequence 1.
    const auto trackStr = [](const pipeline::M2AnimationTrack& t, bool vec) {
        for (const auto& s : t.sequences) {
            if (vec ? s.vec3Values.empty() : s.floatValues.empty()) continue;
            char buf[96];
            if (vec) {
                const auto v = s.vec3Values.front();
                std::snprintf(buf, sizeof buf, "(%.2f,%.2f,%.2f)", v.x, v.y, v.z);
            } else {
                std::snprintf(buf, sizeof buf, "%.2f", s.floatValues.front());
            }
            std::string out = buf;
            const size_t keys = vec ? s.vec3Values.size() : s.floatValues.size();
            if (keys > 1) out += "~" + std::to_string(keys) + "k";
            if (t.globalSequence >= 0) out += " gs" + std::to_string(t.globalSequence);
            return out;
        }
        return std::string("-");
    };
    std::printf("lights: %zu\n", model.lights.size());
    for (size_t i = 0; i < model.lights.size(); ++i) {
        const auto& l = model.lights[i];
        std::printf("  [%zu] type %u bone %d pos (%.2f,%.2f,%.2f)\n"
                    "       ambient %s x %s  diffuse %s x %s  atten %s..%s  visible %s\n",
                    i, l.type, l.bone, l.position.x, l.position.y, l.position.z,
                    trackStr(l.ambientColor, true).c_str(), trackStr(l.ambientIntensity, false).c_str(),
                    trackStr(l.diffuseColor, true).c_str(), trackStr(l.diffuseIntensity, false).c_str(),
                    trackStr(l.attenuationStart, false).c_str(), trackStr(l.attenuationEnd, false).c_str(),
                    trackStr(l.visibility, false).c_str());
    }
    std::printf("billboard bones:");
    for (size_t i = 0; i < model.bones.size(); ++i)
        if (model.bones[i].flags & 0x78) std::printf(" %zu(0x%x)", i, model.bones[i].flags);
    std::printf("\n");
    return 0;
}
