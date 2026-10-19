#!/usr/bin/env python3
"""Count changes the client makes to the renderers' scene directly.

The client changes what is drawn through rendering::SceneCommands
(include/rendering/scene_commands.hpp), and the changes are made at one point
of the frame, the start of the renderer's update. From there to the end of the
frame the instances are only read - which is what a renderer drawing from a
snapshot on its own thread needs. A call straight to a renderer's instance
methods from the client changes the scene wherever it happens to run, and
nothing says so: the frame works, until the scene is read on another thread.
So the count of such calls outside src/rendering is held at zero.

Calls to SceneCommands go through references (`chars.playAnimation(...)`);
an arrow to one of the methods below is a call on a renderer.

    tools/scene_write_check.py

Prints "<N> direct changes to the scene from the client", then each one.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCANNED = ("src/core", "src/ui", "src/game", "src/addons")

# The instance methods of CharacterRenderer, M2Renderer and WMORenderer that
# change the scene, as SceneCommands offers them.
METHODS = (
    "createInstance", "createInstanceWithMatrix", "removeInstance", "removeInstances",
    "unloadModelIfUnused",
    "setInstancePosition", "setInstanceRotation", "setInstanceTorsoYaw", "setInstanceVisible",
    "setInstanceOpacity", "startFadeIn", "playAnimation", "setInstanceAnimationSpeed",
    "attachWeapon", "detachWeapon", "moveWeapon", "attachWeaponEffect", "detachWeaponEffects",
    "setTextureSlotOverride", "clearTextureSlotOverride", "setGroupTextureOverride",
    "setActiveGeosets", "setDrawSkinExtra", "addLookCompanion",
    "setInstanceTransform", "setInstanceAnimation", "setInstanceAnimationFrozen",
    "restartInstanceAnimation", "playInstanceAnimation", "setInstanceHighlight",
    "clearInstanceHighlights", "setSkipCollision", "setSkipWallCollision", "setInteriorLight",
    "setInstanceIsGameObject", "addDoodadToInstance", "setInstanceIsTransport",
    "setInstanceHidden", "setInstanceDoodadAnimation",
)
CALL = re.compile(r"->\s*(" + "|".join(METHODS) + r")\s*\(")
LINE_COMMENT = re.compile(r"//.*$")


def calls_in(path: pathlib.Path) -> list[tuple[int, str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [(number, line.strip()) for number, line in enumerate(text.splitlines(), 1)
            if CALL.search(LINE_COMMENT.sub("", line))]


def main() -> int:
    found = []
    scanned = 0
    for top in SCANNED:
        for path in sorted((ROOT / top).rglob("*")):
            if path.suffix not in (".cpp", ".hpp", ".h", ".cc", ".inl"):
                continue
            scanned += 1
            for number, line in calls_in(path):
                found.append(f"{path.relative_to(ROOT).as_posix()}:{number}: {line}")
    # The facade makes these calls itself; seeing them there is what shows
    # the matcher still recognises one.
    facade = ROOT / "src" / "rendering" / "scene_commands.cpp"
    inside = len(calls_in(facade)) if facade.exists() else 0
    print(f"{len(found)} direct changes to the scene from the client")
    print(f"{scanned} files scanned; {inside} such calls inside SceneCommands itself")
    for entry in found:
        print(f"  {entry}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
