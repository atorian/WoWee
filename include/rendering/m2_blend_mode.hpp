#pragma once

/// What an M2 material's blend mode means for the pipeline it draws on.
///
/// A batch whose texture carries no alpha channel used to be forced onto the
/// cutout pipeline whenever its blend mode was 2 or higher. For blend mode 2 -
/// alpha blend - that is reasonable: blending by an alpha that is 1 everywhere
/// is the same as drawing opaque, so cutting out at least gives the artist's
/// silhouette a chance.
///
/// For the additive modes it is wrong, and destroys the effect. Additive does
/// not use alpha for transparency at all - black is what disappears, because
/// adding zero changes nothing. A glow card is authored exactly that way: an
/// opaque texture, bright in the middle, black at the edges.
///
/// Orgrimmar's bonfire is the case that showed it. Its glow is
/// GENERICGLOW_ALPHA_128.BLP - named for an alpha it does not have, alphaDepth
/// 0 in the file - on a material with blend mode 4. Forced to cutout, every
/// texel passed the test and the flame rendered as a flat opaque grey disc
/// sitting behind the logs.

#include <cstddef>
#include <cstdint>

namespace wowee::rendering {

/// M2 material blend modes, as stored.
enum M2BlendMode : uint8_t {
    M2_BLEND_OPAQUE = 0,
    M2_BLEND_ALPHA_KEY = 1,   ///< cutout: alpha tested against a threshold
    M2_BLEND_ALPHA = 2,       ///< blended by the texture's alpha
    M2_BLEND_ADD = 3,         ///< added to what is behind; black is invisible
    M2_BLEND_ADD_ALPHA = 4,   ///< additive, scaled by alpha where there is one
    M2_BLEND_MODULATE = 5,
    M2_BLEND_MODULATE2X = 6,
};

/// The pipelines an M2 batch can be drawn on, one per distinct blend state.
enum class M2BlendPipeline : uint8_t {
    Opaque,
    AlphaTest,
    Alpha,
    NoAlphaAdd,   ///< ONE, ONE: the texture's alpha does not attenuate it
    AlphaAdd,     ///< SRC_ALPHA, ONE
    Modulate,
    Modulate2x,
};
inline constexpr size_t kM2BlendPipelineCount = static_cast<size_t>(M2BlendPipeline::Modulate2x) + 1;

/// What fog turns a pixel into, by blend mode (WoW.exe 0xa45390): the fog's
/// colour where the pixel replaces what is behind it, and for the others the
/// colour that leaves what is behind unchanged - black added, white
/// multiplied, grey doubled.
enum class M2FogMode : int32_t {
    None = 0,
    FogColor = 1,
    Black = 2,
    White = 3,
    Grey = 4,
};
inline M2FogMode m2FogMode(M2BlendPipeline pipeline) {
    switch (pipeline) {
        case M2BlendPipeline::NoAlphaAdd:
        case M2BlendPipeline::AlphaAdd: return M2FogMode::Black;
        case M2BlendPipeline::Modulate: return M2FogMode::White;
        case M2BlendPipeline::Modulate2x: return M2FogMode::Grey;
        default: return M2FogMode::FogColor;
    }
}

/// The alpha below which the client discards a pixel (WoW.exe 0x81fb10): none
/// for Opaque, 224/255 for AlphaKey, 1/255 for every blended mode.
inline float m2AlphaRef(M2BlendPipeline pipeline) {
    switch (pipeline) {
        case M2BlendPipeline::Opaque: return 0.0f;
        case M2BlendPipeline::AlphaTest: return 224.0f / 255.0f;
        default: return 1.0f / 255.0f;
    }
}

/// The blend mode a batch is drawn with.
///
/// Spell visuals are effect overlays painted on black, so
/// their opaque and alpha-keyed layers are drawn additively, scaled by alpha
/// like mode 4. Every other mode is drawn as authored. Mod (5) used to be
/// turned additive here as well, and Mod only darkens: dark smoke, shadow and
/// void effects glowed instead.
inline uint8_t m2DrawBlendMode(uint8_t blendMode, bool effectOverlay) {
    if (effectOverlay && blendMode <= M2_BLEND_ALPHA_KEY) return M2_BLEND_ADD_ALPHA;
    return blendMode;
}

/// The pipeline a drawn blend mode maps to. Mode 3 is NoAlphaAdd and must not
/// share mode 4's SRC_ALPHA factor: a mode 3 texture with a zero or partial
/// alpha channel disappeared or dimmed. Modes past 6 are drawn additively.
inline M2BlendPipeline m2BlendPipeline(uint8_t drawBlendMode) {
    switch (drawBlendMode) {
        case M2_BLEND_OPAQUE: return M2BlendPipeline::Opaque;
        case M2_BLEND_ALPHA_KEY: return M2BlendPipeline::AlphaTest;
        case M2_BLEND_ALPHA: return M2BlendPipeline::Alpha;
        case M2_BLEND_ADD: return M2BlendPipeline::NoAlphaAdd;
        case M2_BLEND_MODULATE: return M2BlendPipeline::Modulate;
        case M2_BLEND_MODULATE2X: return M2BlendPipeline::Modulate2x;
        default: return M2BlendPipeline::AlphaAdd;
    }
}

/// RGB multiplication has no alpha silhouette or black colour key.
inline bool m2BlendIsModulate(uint8_t blendMode) {
    return blendMode == M2_BLEND_MODULATE || blendMode == M2_BLEND_MODULATE2X;
}

/// Additive modes need no alpha channel: black is the transparent colour.
inline bool m2BlendIsAdditive(uint8_t blendMode) {
    return blendMode == M2_BLEND_ADD || blendMode == M2_BLEND_ADD_ALPHA;
}

/// Should this batch be alpha tested?
///
/// Alpha key is, that being what it means - but only where there is alpha to
/// key on. A blended batch whose texture has no alpha is tested as a fallback,
/// because blending by a missing alpha draws it solid. An additive batch never
/// is: it has nothing to test and does not need one.
///
/// An alpha key over a texture with no transparent texel is the case worth
/// naming. Every texel passes, so the test decides nothing - but it still puts
/// the batch on the cutout pipeline, where alpha-to-coverage decides how much
/// of each sample the batch covers. Tirisfal's canopy trees are two batches,
/// and their trunk is exactly this: TirrisFallCanopyTree01_Trunk is DXT1 with
/// no punch-through block anywhere in it, drawn alpha-keyed.
///
/// Canopytree07, the one of the seven whose trunk is plain opaque, was read
/// here as the one that rendered right. It was not. Its trunk samples
/// SilverPineTree01TrunkSkin, which does carry alpha, and the foliage rule in
/// m2_renderer_render.cpp forced it onto the cutout pipeline whatever this
/// function said - so it had the same hole through its middle that every
/// Silverpine tree had. Opaque was not reaching the pipeline at all until that
/// rule learned to ask BLPImage::alphaIsSilhouette.
///
/// Safe to lean on, because hasAlpha is measured rather than guessed:
/// BLPImage::hasTransparency walks level 0 and, for DXT1, counts a block only
/// when it is in punch-through mode and a texel actually selects index 3.
inline bool m2BatchNeedsAlphaTest(uint8_t blendMode, bool hasAlpha) {
    if (blendMode == M2_BLEND_ALPHA_KEY) return hasAlpha;
    if (m2BlendIsAdditive(blendMode) || m2BlendIsModulate(blendMode)) return false;
    return blendMode >= M2_BLEND_ALPHA && !hasAlpha;
}

/// What a doodad batch's material and its model's classification say.
///
/// Nothing here keys out black. The client has no colour key: a batch is cut
/// by its texture's alpha against the AlphaRef of its blend mode, and black is
/// invisible only where the blend adds it. Keying black on textures whose
/// names spelled fire cut holes in the dark parts of campfire rocks, forge
/// coals and bonfire logs, all of which the client draws solid.
struct M2BatchMaterialInputs {
    uint8_t blendMode = M2_BLEND_OPAQUE;
    bool hasAlpha = false;
    bool alphaIsSilhouette = false;
    bool spellEffect = false;
    bool groundDetail = false;
    bool foliageLike = false;
};

/// How a doodad batch is drawn. None of it changes from frame to frame or
/// between instances, so it is decided once, kept on the batch and written
/// into the material buffer when the model loads. The draw loops used to work
/// it out per draw and write it into a buffer every instance shares, which the
/// GPU may still have been reading for the frame before.
struct M2BatchDrawState {
    uint8_t drawBlendMode = M2_BLEND_OPAQUE;
    bool forceCutout = false;     ///< drawn on the opaque cutout pipeline
    /// 0 none, 1 cutout, 2 foliage, 3 ground clutter, 4 the material's own
    /// alpha key - cut at 224/255, where a WotLK client cuts GxBlend_AlphaKey.
    int32_t alphaTest = 0;
    bool litOverride = false;     ///< ground clutter is lit whatever its material says
};

inline M2BatchDrawState m2BatchDrawState(const M2BatchMaterialInputs& in) {
    M2BatchDrawState s;
    const bool effectOverlay = in.spellEffect;

    // A batch the artist marked opaque is not asking for any of this. Blend
    // mode 0 means the alpha channel goes unread, and M2 textures are
    // routinely atlases whose alpha belongs to another layer or another model
    // - so keying on it eats surfaces that were painted solid.
    //
    // Silverpine's trees are the case that found it. Their trunk is one opaque
    // batch sampling the right-hand panel of SilverPineTree01TrunkSkin, and
    // that panel carries a stale alpha hole across its middle - 24% of the
    // texels the trunk's own triangles cover, with unbroken bark painted
    // underneath. Forced to the cutout pipeline, where alpha becomes coverage,
    // the trunk lost its midsection and left the canopy floating over the
    // stump. 133 foliage models were holed the same way - Tirisfal's
    // canopytree07, the Plaguelands and Stonetalon pines, Eversong and
    // Silvermoon, Nagrand, Crystalsong, Ruby Sanctum, the Storm Peaks, and
    // every Zangarmarsh mushroom cap.
    //
    // Where the alpha really is the silhouette - a card drawn on a black
    // backing, or DXT1 punch-through, which stores no colour under a
    // transparent texel at all - the cutout still stands, over the 15 models
    // that need it: Swamp of Sorrows' canopies, the quilboar thorn cards, Loch
    // Modan's leaves and the lamps whose glass is keyed out. Measured from the
    // texture by alphaIsSilhouette, not guessed from its name.
    const bool opaqueByMaterial = in.blendMode == M2_BLEND_OPAQUE;
    const bool foliageCutout = in.foliageLike && !in.spellEffect &&
                               in.blendMode <= M2_BLEND_ADD &&
                               (!opaqueByMaterial || in.alphaIsSilhouette);

    // Only the opaque pass cuts out, and it draws opaque and alpha-keyed
    // batches alone.
    s.forceCutout = in.blendMode <= M2_BLEND_ALPHA_KEY && !effectOverlay &&
                    (in.groundDetail || foliageCutout ||
                     m2BatchNeedsAlphaTest(in.blendMode, in.hasAlpha));

    s.drawBlendMode = s.forceCutout ? static_cast<uint8_t>(M2_BLEND_ALPHA_KEY)
                                    : m2DrawBlendMode(in.blendMode, effectOverlay);
    // A material the artist keyed (blend mode 1) is cut where the original
    // client cuts it: 224/255, not a half. Dalaran's inn windows are the case
    // that showed it - dalaran_inn_window.m2 keys DAL_WINDOWCUTOUT, whose holes
    // sit at alpha 0.5-0.6, so cut at 0.4 the lattice drew as a solid pane.
    // Foliage and ground clutter keep the cutoffs tuned for them.
    const bool materialKey = in.blendMode == M2_BLEND_ALPHA_KEY && in.hasAlpha;
    if (s.forceCutout) {
        s.alphaTest = in.groundDetail ? 3 : (foliageCutout ? 2 : (materialKey ? 4 : 1));
        s.litOverride = in.groundDetail;
    } else {
        s.alphaTest = m2BatchNeedsAlphaTest(in.blendMode, in.hasAlpha) ? 1 : 0;
    }
    return s;
}

/// Characters and mounts honor opaque material alpha, even on DXT5 atlases.
inline bool characterBatchNeedsAlphaTest(uint16_t mode, bool hasAlpha,
                                        bool hair, bool scene) {
    if (scene) return mode == M2_BLEND_ALPHA_KEY;
    if (hair || mode == M2_BLEND_ALPHA_KEY) return true;
    const bool noSilhouette = mode == M2_BLEND_ADD || mode == M2_BLEND_ADD_ALPHA ||
                              mode == M2_BLEND_MODULATE || mode == M2_BLEND_MODULATE2X ||
                              mode == 7;
    return mode >= 2 && !hasAlpha && !noSilhouette;
}

/// The pipelines a character, creature or attachment batch can be drawn on.
enum class CharacterBlendPipeline : uint8_t {
    Opaque,
    AlphaTest,
    Alpha,         ///< blended, no depth write
    Translucent,   ///< blended, depth written: a fading character's solid parts
    Additive,      ///< SRC_ALPHA, ONE
    Modulate,
    Modulate2x,
};

struct CharacterBatchBlendInputs {
    uint16_t blendMode = M2_BLEND_OPAQUE;
    uint16_t materialFlags = 0;
    bool effectModel = false;   ///< an enchant visual
    bool hairMaterial = false;
    float opacity = 1.0f;       ///< instance fade times the batch's animated alpha
};

/// The pipeline a character batch draws on.
///
/// Mod (5) and Mod2x (6) multiply what is behind them. Mod2x was drawn
/// additive, which brightens where it should darken below 0.5 and lighten
/// above it, and Mod fell through to the alpha blend, which painted the
/// texture over the surface instead of tinting it.
inline CharacterBlendPipeline characterBlendPipeline(const CharacterBatchBlendInputs& in) {
    // Enchant visuals are glow cards drawn on black. Their materials declare
    // Mod/alpha blending, which would composite that black background as an
    // opaque quad - force additive so only the light adds.
    if (in.effectModel) return CharacterBlendPipeline::Additive;

    // Decided before the fade below. An additive card fades by adding less
    // light, and a modulated one by multiplying by something nearer its
    // identity, both in the shader - so a partial alpha is no reason to divert
    // either to the translucent pipeline, which composites the card's black
    // backing as black. The Darnassus wisps are the case: their glow alpha is
    // an animated pulse, and they showed as black about half the time. Add
    // (4) is a mode that used to reach the alpha blend, which composited a
    // glow card's black backing as a black quad: every material on Wisp.m2
    // declares it.
    switch (in.blendMode) {
        case M2_BLEND_ADD:
        case M2_BLEND_ADD_ALPHA:
        case 7:
            return CharacterBlendPipeline::Additive;
        case M2_BLEND_MODULATE: return CharacterBlendPipeline::Modulate;
        case M2_BLEND_MODULATE2X: return CharacterBlendPipeline::Modulate2x;
        default: break;
    }

    if (in.opacity < 0.999f) {
        // Whole-instance fade (ghost form, spawn fade-in): the opaque and
        // alpha-test pipelines have blending disabled, so the fade only shows
        // through a blend pipeline. A batch that must not write depth still
        // must not while fading: the glue screens' cloud and haze cards ask
        // for no depth write (0x10), animate their alpha, and punched a
        // rectangle out of the character behind them through the writing one.
        const bool noDepthWrite = in.blendMode >= M2_BLEND_ALPHA ||
                                  (in.materialFlags & 0x10) != 0;
        return noDepthWrite ? CharacterBlendPipeline::Alpha
                            : CharacterBlendPipeline::Translucent;
    }

    // Hair textures are authored as alpha-cut cards. On the translucent
    // pipeline they form a soft shell around the head.
    if (in.hairMaterial) return CharacterBlendPipeline::AlphaTest;

    switch (in.blendMode) {
        case M2_BLEND_OPAQUE: return CharacterBlendPipeline::Opaque;
        case M2_BLEND_ALPHA_KEY: return CharacterBlendPipeline::AlphaTest;
        default: return CharacterBlendPipeline::Alpha;
    }
}

/// Whether a character batch ignores scene lighting. Additive layers emit
/// their own light and enchant glows must not be tinted by the scene. Mod and
/// Mod2x follow their material's unlit flag, as they do on doodads; they were
/// forced unlit along with every other mode above 2.
inline bool characterBatchUnlit(uint16_t blendMode, uint16_t materialFlags, bool effectModel) {
    if (effectModel || (materialFlags & 0x01) != 0) return true;
    return blendMode >= M2_BLEND_ADD && blendMode != M2_BLEND_MODULATE &&
           blendMode != M2_BLEND_MODULATE2X;
}

}  // namespace wowee::rendering
