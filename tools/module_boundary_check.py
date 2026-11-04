#!/usr/bin/env python3
"""Includes that reach up or sideways across the client's module layers.

    tools/module_boundary_check.py           # fail on a new or a fixed violation
    tools/module_boundary_check.py --prune   # drop fixed ones from the baseline

WHY

src/ and include/ are split into module directories, but nothing stops a file
from including any other. That is how core came to include rendering 99 times
while rendering includes core 112 times, and how game and rendering, game and
ui, ui and addons ended up depending on each other. A cycle like that is why a
module cannot become its own library, why a renderer test needs the whole
application, and why touching one header recompiles half the client.

The existing cycles cannot all be broken at once, so they are recorded in
tools/module_boundary_baseline.txt and allowed. Everything else must point
down the layers below. The baseline only shrinks: an entry that no longer
occurs fails the check too, so a fixed violation cannot quietly come back.

LAYERS

A module may include its own headers and any module on a lower layer. Modules
on the same layer may not include each other.

    0  base, math, platform, third_party
    1  <context>/domain
    2  <context>/services
    3  pipeline, network
    4  auth, collision
    5  game, audio, rendering
    6  ui
    7  addons
    8  app
    9  main

A bounded context is a top-level directory holding domain/ and services/,
such as contacts/. Its domain may include only base and itself, its services
only base, their own domain and themselves, and every module above may use
both. Two contexts do not include each other's services: they share a layer.

game, audio and rendering share a layer on purpose: none of them may include
another (#33). collision is below rendering, which places what it draws in
it; game, which may not include it either, is handed the queries it needs. What rendering needs from the game arrives as plain inputs, and
what the game wants drawn or played goes out through an interface that app
wires up.

`base` and `app` are both under core/ today: base is the infrastructure in
BASE_HEADERS and BASE_SOURCES (what the wowee_base library provides plus
dependency-free headers), app is the rest of core - the composition root. The
split into two directories is issue #34; until then this file is where the
line is drawn.

WHAT IT CANNOT SEE

An include written relative to the including file ("panel.hpp", "../x.hpp")
is not attributed to a module; there are a handful, none crossing modules.
Includes inside #if branches that are never compiled count like any other.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "module_boundary_baseline.txt"

LAYERS = [
    {"base", "math", "platform", "third_party"},
    {"domain"},
    {"services"},
    {"pipeline", "network"},
    {"auth", "collision"},
    {"game", "audio", "rendering"},
    {"ui"},
    {"addons"},
    {"app"},
    {"main"},
]
LAYER_OF = {module: i for i, modules in enumerate(LAYERS) for module in modules}
CONTEXT_LAYERS = {"domain", "services"}


def layer_of(module: str) -> int:
    """A context module such as "contacts/domain" takes its layer's place."""
    return LAYER_OF[module.split("/", 1)[1] if "/" in module else module]

# core/ headers and sources that belong to base rather than app.
BASE_HEADERS = {
    "core/app_clock.hpp",
    "core/config_paths.hpp",
    "core/coordinates.hpp",
    "core/cvar_store.hpp",
    "core/data_paths.hpp",
    "core/event_bus.hpp",
    "core/env.hpp",
    "core/env_flag.hpp",
    "core/frame_budget.hpp",
    "core/local_time.hpp",
    "core/log_privacy.hpp",
    "core/logger.hpp",
    "core/packed_time.hpp",
    "core/memory_monitor.hpp",
    "core/profiler.hpp",
    "core/thread_pool.hpp",
    "core/thread_priority.hpp",
}
BASE_SOURCES = {
    "src/core/app_clock.cpp",
    "src/core/config_paths.cpp",
    "src/core/cvar_store.cpp",
    "src/core/frame_budget.cpp",
    "src/core/logger.cpp",
    "src/core/memory_monitor.cpp",
    "src/core/thread_priority.cpp",
}

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".mm"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.M)


def module_of_header(path: str) -> str | None:
    """Module of an include path such as "game/world.hpp", or None."""
    if path in BASE_HEADERS:
        return "base"
    if "/" not in path:
        return None
    parts = path.split("/")
    if len(parts) > 2 and parts[1] in CONTEXT_LAYERS:
        return f"{parts[0]}/{parts[1]}"
    first = parts[0]
    if first == "core":
        return "app"
    return first if first in LAYER_OF and first not in CONTEXT_LAYERS else None


def module_of_file(rel: str) -> str | None:
    """Module of a file under src/ or include/, given relative to ROOT."""
    if rel in BASE_SOURCES:
        return "base"
    top, rest = rel.split("/", 1)
    if top == "include":
        return module_of_header(rest)
    if "/" not in rest:
        return "main" if rest.startswith("main.") else None
    return module_of_header(rest)


def violations() -> set[str]:
    found = set()
    for top in ("src", "include"):
        for path in sorted((ROOT / top).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            source = module_of_file(rel)
            if source is None:
                continue
            for header in INCLUDE_RE.findall(path.read_text(errors="replace")):
                target = module_of_header(header)
                if target is None or target == source:
                    continue
                if layer_of(target) >= layer_of(source):
                    found.add(f"{rel} -> {header}")
    return found


def read_baseline() -> set[str]:
    if not BASELINE.is_file():
        return set()
    lines = BASELINE.read_text().splitlines()
    return {line.strip() for line in lines if line.strip() and not line.startswith("#")}


def write_baseline(entries: set[str]) -> None:
    header = [line for line in BASELINE.read_text().splitlines() if line.startswith("#")]
    BASELINE.write_text("\n".join(header + sorted(entries)) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--prune", action="store_true",
                        help="remove baseline entries that no longer occur")
    args = parser.parse_args()

    current = violations()
    if not current and not (ROOT / "src" / "core").is_dir():
        print("src/core is not here; finding nothing would mean the scan broke.")
        return 1

    baseline = read_baseline()
    new = sorted(current - baseline)
    fixed = sorted(baseline - current)

    if args.prune:
        if fixed:
            write_baseline(baseline - set(fixed))
        print(f"removed {len(fixed)} fixed entr{'y' if len(fixed) == 1 else 'ies'} "
              f"from {BASELINE.relative_to(ROOT)}")
        for entry in fixed:
            print(f"  {entry}")
        return 0

    print(f"{len(current)} known violation(s), {len(baseline)} in the baseline")
    if new:
        print(f"\n{len(new)} new include(s) against the layering "
              f"(see the LAYERS table in {pathlib.Path(__file__).name}):")
        for entry in new:
            print(f"  {entry}")
    if fixed:
        print(f"\n{len(fixed)} baseline entr{'y' if len(fixed) == 1 else 'ies'} "
              f"no longer occurring; run with --prune to drop "
              f"{'it' if len(fixed) == 1 else 'them'}:")
        for entry in fixed:
            print(f"  {entry}")
    return 1 if new or fixed else 0


if __name__ == "__main__":
    sys.exit(main())
