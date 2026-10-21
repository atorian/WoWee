#!/usr/bin/env python3
"""Count world recording paths that read mutable producer-side scene state.

Colour, depth, particles, water, grass, sky, weather and overlays must consume
extracted draw data. GPU preparation may update frame slots and acceleration
structures, but must not walk live instances, streaming lists or simulations.
Interface composites consume extracted data too. sweep_guard holds the count
at zero; the list of checked definitions makes missing coverage visible.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FILES = tuple("src/rendering/" + name + ".cpp" for name in (
    "m2_renderer", "m2_renderer_render", "m2_renderer_particles", "m2_renderer_instance",
    "m2_renderer_posed_emitters", "wmo_renderer", "character_renderer", "terrain_renderer",
    "terrain_bake", "distant_terrain", "water_renderer", "grass_renderer", "sky_system", "skybox", "clouds",
    "starfield", "lens_flare", "precipitation", "ocean_waves", "water_wake", "mount_dust",
    "swim_effects", "lightning", "charge_effect", "quest_marker_renderer", "footprint_renderer",
    "overlay_system", "volumetric_fog", "rt_scene", "renderer", "renderer_results", "minimap",
    "interface_frame", "character_preview", "world_map/composite_renderer"))
DEFINITION = re.compile(r"^\w[\w:<>,\s\*&]*\b(\w+)::(\w+)\s*\(", re.M)
LIVE = {
    "M2Renderer": r"scene_|models|frameLights_|cachedCamPos_|skyTimeOfDayHours_",
    "WMORenderer": r"scene_|loadedModels|frameLights_",
    "CharacterRenderer": r"scene_|models|frameLights_|normalMappingEnabled_|normalMapStrength_",
    "TerrainRenderer": r"chunks|chunkAt_|geometryPages_|shadowTiles_|bakeQueue_",
    "TerrainBake": r"atlases_",
    "DistantTerrain": r"gpu_|pending_|map_|source_|setMap|update",
    "WaterRenderer": r"drawn_|gameUBO_|inView_|renderingEnabled",
    "GrassRenderer": r"population_|profiles_|cullDistance_",
    "SkySystem": r"proceduralStarsEnabled_|debugSkyMode_",
    "Clouds": r"windOffset_|density_|enabled_",
    "StarField": r"twinkleTime|renderingEnabled",
    "Skybox": r"renderingEnabled|timeOfDay",
    "LensFlare": r"enabled|intensityMultiplier",
    "Precipitation": r"frame_|layers_|water_|time_|tint_|drawnCount_",
    "OceanWaves": r"sea_|cascades_|pendingSeconds_",
    "WaterWake": r"cornerTexel_|pendingShift_|pendingSteps_|stillPending_|moverCount_|place_|shape_|head_",
    "MountDust": r"particles",
    "SwimEffects": r"rippleVertexData|bubbleVertexData|insectVertexData",
    "Lightning": r"enabled|bolts|flash|storm_|eye_",
    "ChargeEffect": r"trail_|dustPuffs_",
    "QuestMarkerRenderer": r"markers_",
    "FootprintRenderer": r"prints_",
    "OverlaySystem": r"selCircleVisible_|selCirclePos_|selCircleRadius_|selCircleColor_|world",
    "RtScene": r"sourceMeshes_|sourceInstances_|changes_|extractChanges",
    "Renderer": r"collision_|liquidScene_|frameSink_|screenshotRequests_|worldEpoch_",
    "Minimap": r"assetManager|mapName|trsLookup|readyTiles_|tileTextureCache|viewRadius|rotateWithCamera|haveRect_|opacity_",
    "InterfaceRecorder": r"ImGui|ImGui_ImplVulkan_RenderDrawData",
    "CharacterPreview": r"camera|modelLoaded_|compositeRequested_",
    "CompositeRenderer": r"assetManager|zoneTextureSlots_|pendingCompositeIdx_|prepared_",
}
EXTRA = {"prepareRender", "dispatchCullCompute", "dispatchCull", "cullShadowCasters"}
PRODUCER_CALL = re.compile(r"->(?:extract\w*|gather\w*|syncRtScene|updateInView)\s*\(")
SCENE = re.compile(r"\bscene_\b")
LINE_COMMENT = re.compile(r"//.*$")


def body_of(text: str, start: int) -> tuple[int, str]:
    """The braced body of the definition starting at `start`, and the offset
    it begins at."""
    open_at = text.index("{", start)
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return open_at, text[open_at:i + 1]
    raise ValueError("unbalanced braces")


def main() -> int:
    functions = []
    found = []
    for name in FILES:
        path = ROOT / name
        if not path.exists():
            print(f"{name} is listed and missing")
            return 1
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in DEFINITION.finditer(text):
            cls, method = match.groups()
            if cls not in LIVE or not (method.startswith(("record", "render")) or method in EXTRA or
                                       cls == "RtScene" and method in ("update", "applyChanges")):
                continue
            qualified = f"{cls}::{method}"
            live = re.compile(r"(?<![.\w])(?:" + LIVE[cls] + r")\b")
            functions.append(qualified)
            offset, body = body_of(text, match.end())
            first_line = text.count("\n", 0, offset) + 1
            for number, line in enumerate(body.splitlines(), first_line):
                code = LINE_COMMENT.sub("", line)
                if live.search(code) or cls == "Renderer" and (PRODUCER_CALL.search(code) or
                        re.search(r"\b(?:extractFrameInputs|computeLightSpaceMatrix)\s*\(", code)):
                    found.append(f"{name}:{number}: {qualified}: {line.strip()}")
    print(f"{len(found)} reads of the scene while recording")
    print(f"{len(functions)} recording functions: {', '.join(functions)}")
    for entry in found:
        print(f"  {entry}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
