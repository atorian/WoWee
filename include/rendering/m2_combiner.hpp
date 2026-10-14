#pragma once

// How an M2 batch combines its textures, as the 3.3.5a client decides it at
// skin load (WoW.exe 12340: shader id rewrite 0x836980, effect pick 0x836600;
// render spec section 6.1).
//
// Every batch draws through a vertex shader that says where each stage's
// coordinates come from and a pixel combiner that says how the stages and
// the vertex colour meet. Both follow from the batch's shader id, which the
// client rebuilds for models without flag 0x08 and reads through the model's
// textureCombinerCombos for those with it.

#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// The pixel combiners 3.3.5a reaches, by the maths they do (t0, t1 the
/// stages, v the lit vertex colour).
enum class M2Combiner : uint8_t {
    Opaque,          ///< t0 v,           a = v.a
    Mod,             ///< t0 v,           a = t0.a v.a
    Decal,           ///< lerp(t0, v, v.a)
    Add,             ///< t0 + v
    Mod2x,           ///< 2 t0 v
    Fade,            ///< lerp(v, t0, v.a)
    OpaqueOpaque,    ///< t0 t1 v,        a = v.a
    OpaqueMod,       ///< t0 t1 v,        a = t1.a v.a
    OpaqueAdd,       ///< t0 v + t1,      a = v.a + t1.a
    OpaqueMod2x,     ///< 2 t0 t1 v,      a = 2 t1.a v.a
    OpaqueMod2xNA,   ///< 2 t0 t1 v,      a = v.a
    OpaqueAddNA,     ///< t0 v + t1,      a = v.a
    ModOpaque,       ///< t0 t1 v,        a = t0.a v.a
    ModMod,          ///< t0 t1 v,        a = t0.a t1.a v.a
    ModAdd,          ///< t0 v + t1,      a = t0.a v.a + t1.a
    ModMod2x,        ///< 2 t0 t1 v,      a = 2 t0.a t1.a v.a
    ModMod2xNA,      ///< 2 t0 t1 v,      a = t0.a v.a
    ModAddNA,        ///< t0 v + t1,      a = t0.a v.a
    AddMod,          ///< (v + t0) t1,    a = (v.a + t0.a) t1.a
    Mod2xMod2x,      ///< 4 t0 t1 v,      a = 4 t0.a t1.a v.a
    OpaqueMod2xNAAlpha,    ///< v lerp(2 t0 t1, t0, t0.a), a = v.a (shader 0x8001)
    OpaqueAddAlpha,        ///< t0 v + t1 t1.a,            a = v.a (0x8002)
    OpaqueAddAlphaAlpha,   ///< v (t0 + (1 - t0.a) t1 t1.a), a = v.a (0x8003)
};

/// Where a stage's texture coordinates come from.
enum class M2StageCoords : uint8_t {
    Uv0,          ///< the first UV set, through the stage's UV animation
    Uv1,          ///< the second UV set, through the stage's UV animation
    Environment,  ///< the sphere map, made in the vertex shader
    Uv0Still,     ///< the first UV set with no UV animation (Diffuse_Env_T2's quirk)
};

/// A batch's shading.
struct M2BatchShading {
    bool drawn = true;           ///< shader 0x8000 is not drawn
    M2Combiner combiner = M2Combiner::Mod;
    uint8_t stages = 1;          ///< 1 or 2
    M2StageCoords coords0 = M2StageCoords::Uv0;
    M2StageCoords coords1 = M2StageCoords::Uv1;
};

namespace m2_combiner_detail {
// Stage op codes, as the shader id and the combo table carry them.
constexpr uint16_t kOpOpaque = 0, kOpMod = 1, kOpDecal = 2, kOpAdd = 3, kOpMod2x = 4,
                   kOpFade = 5, kOpMod2xNA = 6, kOpAddNA = 7;

// One-stage pixel shaders: ops 6 and 7 fall back to Mod.
inline M2Combiner oneStage(uint16_t op) {
    switch (op) {
        case kOpOpaque: return M2Combiner::Opaque;
        case kOpDecal: return M2Combiner::Decal;
        case kOpAdd: return M2Combiner::Add;
        case kOpMod2x: return M2Combiner::Mod2x;
        case kOpFade: return M2Combiner::Fade;
        default: return M2Combiner::Mod;
    }
}

// Two-stage pixel shaders; a pair the client has no shader for retries as
// shader 0x11, Combiners_Mod_Mod.
inline M2Combiner twoStage(uint16_t op0, uint16_t op1) {
    const auto fromOpaqueOrMod = [&](bool mod) {
        switch (op1) {
            case kOpOpaque: return mod ? M2Combiner::ModOpaque : M2Combiner::OpaqueOpaque;
            case kOpAdd: return mod ? M2Combiner::ModAdd : M2Combiner::OpaqueAdd;
            case kOpMod2x: return mod ? M2Combiner::ModMod2x : M2Combiner::OpaqueMod2x;
            case kOpMod2xNA: return mod ? M2Combiner::ModMod2xNA : M2Combiner::OpaqueMod2xNA;
            case kOpAddNA: return mod ? M2Combiner::ModAddNA : M2Combiner::OpaqueAddNA;
            default: return mod ? M2Combiner::ModMod : M2Combiner::OpaqueMod;  // Mod, Decal, Fade
        }
    };
    switch (op0) {
        case kOpOpaque: return fromOpaqueOrMod(false);
        case kOpMod: return fromOpaqueOrMod(true);
        case kOpAdd: if (op1 == kOpMod) return M2Combiner::AddMod; break;
        case kOpMod2x:
            if (op1 == kOpMod) return M2Combiner::ModMod2x;
            if (op1 == kOpMod2x) return M2Combiner::Mod2xMod2x;
            break;
        default: break;
    }
    return M2Combiner::ModMod;
}
} // namespace m2_combiner_detail

/// A batch's shading from what the model and its skin say.
///
/// `modelFlags` the model header's flags; `combos` its textureCombinerCombos
/// (empty without flag 0x08); `shaderId`, `textureCount` the skin batch's;
/// `blendMode` its material's; `coord0`, `coord1` its stages' entries in
/// textureCoordLookup (-1 is the sphere environment map).
inline M2BatchShading m2BatchShading(uint32_t modelFlags, const std::vector<uint16_t>& combos,
                                     uint16_t shaderId, uint16_t textureCount, uint16_t blendMode,
                                     int16_t coord0, int16_t coord1) {
    using namespace m2_combiner_detail;
    M2BatchShading out;
    out.stages = textureCount >= 2 ? 2 : 1;
    // A coordinate entry above 2 - the -1 of the environment map read
    // unsigned - is the sphere map.
    const bool env0 = static_cast<uint16_t>(coord0) > 2;
    bool env1 = static_cast<uint16_t>(coord1) > 2;

    if (shaderId & 0x8000) {
        // The fixed shaders: a texture and a reflection over it.
        switch (shaderId) {
            case 0x8001: out.combiner = M2Combiner::OpaqueMod2xNAAlpha; break;
            case 0x8002: out.combiner = M2Combiner::OpaqueAddAlpha; break;
            case 0x8003: out.combiner = M2Combiner::OpaqueAddAlphaAlpha; break;
            default: out.drawn = false; break;
        }
        out.stages = 2;
        out.coords0 = M2StageCoords::Uv0;
        out.coords1 = M2StageCoords::Environment;
        return out;
    }

    uint16_t op0 = kOpMod, op1 = kOpOpaque;
    if (!(modelFlags & 0x08)) {
        // Without the flag the file's shader id is not used at all.
        op0 = blendMode != 0 ? kOpMod : kOpOpaque;
        op1 = kOpOpaque;
        env1 = false;
    } else {
        op0 = shaderId < combos.size() ? (combos[shaderId] & 7) : kOpMod;
        if (out.stages == 2) op1 = shaderId + 1u < combos.size() ? (combos[shaderId + 1u] & 7) : kOpMod;
        if (blendMode == 0) op0 = kOpOpaque;
    }

    if (out.stages == 1) {
        out.combiner = oneStage(op0);
        out.coords0 = env0 ? M2StageCoords::Environment
                           : (coord0 != 0 ? M2StageCoords::Uv1 : M2StageCoords::Uv0);
        return out;
    }
    out.combiner = twoStage(op0, op1);
    // Diffuse_T1_T2, _T1_Env, _Env_T2, _Env_Env: two stages take the two UV
    // sets in order; Diffuse_Env_T2 samples its second with UV0 and no UV
    // animation.
    out.coords0 = env0 ? M2StageCoords::Environment : M2StageCoords::Uv0;
    if (env1) out.coords1 = M2StageCoords::Environment;
    else out.coords1 = env0 ? M2StageCoords::Uv0Still : M2StageCoords::Uv1;
    return out;
}

/// A stage's coordinate source as m2.vert reads it: 0 UV0, 1 UV1, -1 the
/// sphere map, 2 UV0 without UV animation.
inline int8_t m2StageCoordsPush(M2StageCoords coords) {
    switch (coords) {
        case M2StageCoords::Uv1: return 1;
        case M2StageCoords::Environment: return -1;
        case M2StageCoords::Uv0Still: return 2;
        default: return 0;
    }
}

/// Whether the combiner reads the second stage.
inline bool m2CombinerUsesSecondStage(M2Combiner c) {
    return c >= M2Combiner::OpaqueOpaque;
}

} // namespace wowee::rendering
