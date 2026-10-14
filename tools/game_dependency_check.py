#!/usr/bin/env python3
"""The game layer reaching for the client's presentation.

    tools/game_dependency_check.py

WHY

The game layer (src/game, include/game) is the protocol and the state it
builds: who is where, what they cast, what the player carries. How any of it
looks or sounds is the client's, in core/, rendering/, audio/ and ui/. The
game says what happened on core::EventBus (game/game_events.hpp) and asks for
what it needs through interfaces it is handed (GameServices,
TransportScene); see AGENTS.md, "Layers".

Each include of the renderer, the sound, the interface or the Application
from the game is a place where that broke, and every one compiles without
complaint because the client target has every include path there is.

WHAT IT DOES

Reads the direct quoted includes of every file in src/game and include/game
and reports the ones that reach rendering/, audio/, ui/, addons/ or a core/
header that is the client's rather than infrastructure. sweep_guard holds the
count to what it was when the check was written; it only goes down.

Shared constants that happen to live under rendering/ (the AnimationData ids,
the movement step limits) are listed apart: they are data both sides read,
waiting for a neutral home, not presentation the game drives.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

FORBIDDEN = ("rendering/", "audio/", "ui/", "addons/")
# core/ headers that are infrastructure the game may use.
CORE_ALLOWED = {
    "core/logger.hpp", "core/profiler.hpp", "core/env_flag.hpp", "core/thread_pool.hpp",
    "core/coordinates.hpp", "core/local_time.hpp", "core/data_paths.hpp",
    "core/memory_monitor.hpp", "core/event_bus.hpp", "core/app_clock.hpp",
    "core/config_paths.hpp", "core/https_get.hpp", "core/cvar_store.hpp",
    "core/string_util.hpp",
}
# Data both sides read, under rendering/ for now.
SHARED_CONSTANTS = {
    "rendering/animation/animation_ids.hpp",
    "rendering/movement_limits.hpp",
}

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def main() -> int:
    files = sorted(list((ROOT / "src/game").rglob("*.cpp")) + list((ROOT / "include/game").rglob("*.hpp")))
    reaches = []
    shared = []
    for path in files:
        text = path.read_text(errors="ignore")
        rel = path.relative_to(ROOT).as_posix()
        for inc in INCLUDE.findall(text):
            if inc in SHARED_CONSTANTS:
                shared.append((rel, inc))
            elif inc.startswith(FORBIDDEN) or (inc.startswith("core/") and inc not in CORE_ALLOWED):
                reaches.append((rel, inc))

    print(f"{len(files)} game layer files\n")
    print(f"{len(reaches)} includes of the client's presentation from the game:")
    if not reaches:
        print("  (none)")
    by_target = {}
    for rel, inc in reaches:
        by_target.setdefault(inc, []).append(rel)
    for inc in sorted(by_target, key=lambda k: (-len(by_target[k]), k)):
        print(f"  {inc:42} {len(by_target[inc]):3}  {', '.join(sorted(set(by_target[inc])))}")
    print(f"\n{len(shared)} includes of shared constants under rendering/ (not counted):")
    for rel, inc in shared:
        print(f"  {inc:42} {rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
