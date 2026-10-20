#!/usr/bin/env python3
"""Hold recording at zero reads of mutable game-side frame-global inputs.

Geometry and UI extraction are later stages; this checks the stage-5b boundary
for camera, lighting, weather, character placement and view settings.
"""
import pathlib
import re
import sys

from record_scene_check import body_of

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFINITION = re.compile(r"^\w[\w:<>,\s\*&]*\bRenderer::(\w+)\s*\(", re.M)
EXTRA = {"beginFrame", "endFrame", "updatePerFrameUBO", "computeLightSpaceMatrix",
         "buildFrameGraph", "drawGlares", "logSunDiagnostic", "logViewDistanceDiag"}
LIVE = re.compile(r"(?<![\w.])(?:camera|lightingManager|characterPosition|characterYaw|"
                  r"characterInstanceId|environment_|viewerMotion_|weather_|weatherLight_|"
                  r"wind_|lensDrops_|lensWindOnScreen_|globalTime|lastDeltaTime_|"
                  r"viewDistance_|shadowDistance_|sunGlare_|sunGlareSky_|moonGlare_|"
                  r"shadowsEnabled|worldInView_|terrainEnabled|volumetricFogDensity_|"
                  r"sunShaftsEnabled_)\b")
COMMENTS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"')


def main() -> int:
    path = ROOT / "src/rendering/renderer.cpp"
    text = path.read_text(encoding="utf-8")
    found = []
    functions = []
    for match in DEFINITION.finditer(text):
        name = match[1]
        if name not in EXTRA and not name.startswith(("render", "record")):
            continue
        functions.append(name)
        offset, body = body_of(text, match.end())
        clean = COMMENTS.sub(lambda m: "\n" * m[0].count("\n"), body)
        for number, line in enumerate(clean.splitlines(), text.count("\n", 0, offset) + 1):
            if LIVE.search(line):
                found.append(f"{path.relative_to(ROOT)}:{number}: {name}: {line.strip()}")
    print(f"{len(found)} live frame input reads while recording")
    print(f"{len(functions)} recording functions checked")
    for entry in found:
        print(f"  {entry}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
