#!/usr/bin/env python3
"""Count reads of the scene from the renderers' recording.

A pass is two steps: a cull, which reads the scene - the instances the game
moves, animates and removes - and copies what the pass needs of each one it
keeps into a list; and the recording, which turns that list into commands and
reads nothing else. A renderer recording on its own thread, from a snapshot,
can only ever read the list: the game thread is changing the scene meanwhile.
So no recording function may reach the scene, and the count of places that do
is held at zero.

The recording functions are the M2 and WMO renderers' `record*` members
(M2Renderer::recordView, recordTransparentPass, recordShadowCasters,
recordM2Particles, recordM2Ribbons; WMORenderer::recordView). A reach is any
use of `scene_`, the renderer's instance state.

    tools/record_scene_check.py

Prints "<N> reads of the scene while recording", the functions it found,
then each read.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FILES = (
    "src/rendering/m2_renderer.cpp", "src/rendering/m2_renderer_render.cpp",
    "src/rendering/m2_renderer_particles.cpp", "src/rendering/m2_renderer_instance.cpp",
    "src/rendering/m2_renderer_posed_emitters.cpp", "src/rendering/wmo_renderer.cpp",
)
DEFINITION = re.compile(r"^\w[\w:<>,\s\*&]*\b(M2Renderer|WMORenderer)::(record\w*)\s*\(", re.M)
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
            qualified = f"{match.group(1)}::{match.group(2)}"
            functions.append(qualified)
            offset, body = body_of(text, match.end())
            first_line = text.count("\n", 0, offset) + 1
            for number, line in enumerate(body.splitlines(), first_line):
                if SCENE.search(LINE_COMMENT.sub("", line)):
                    found.append(f"{name}:{number}: {qualified}: {line.strip()}")
    print(f"{len(found)} reads of the scene while recording")
    print(f"{len(functions)} recording functions: {', '.join(functions)}")
    for entry in found:
        print(f"  {entry}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
