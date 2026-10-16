#!/usr/bin/env python3
"""The renderer library reaching for the client.

    tools/render_dependency_check.py

WHY

wowee_render is the renderer without the client: tools/scene_render draws a
scene with it and nothing else. It stays that way only while nothing in it
includes the game, the interface, the sound, the addons or the network - and
an include of one of them compiles without complaint, because the client
target the same files used to belong to has every include path there is. The
link is what would object, and only the scene tool's link, long after.

Before it was split out the renderer read the realm's clock off the
GameHandler, fired addon events, ran the zone's music and owned the camera
controller that reads key bindings. Each of those was one include away.

WHAT IT DOES

Takes wowee_render's sources the way CMakeLists.txt does - src/rendering less
the client's presentation it lists in WOWEE_RENDER_CLIENT_SOURCES and the
directories it filters out - follows their quoted includes through include/
and src/, and reports every file reached that belongs to another module of
the client. core/ is the client's glue except for the header-only pieces of
infrastructure named below, which wowee_base carries.

WHAT IT CANNOT SEE

A dependency that is not an include: a symbol declared by hand. The scene
tool's link would still catch that one.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Modules of the client the renderer must not reach.
FORBIDDEN = ("game/", "ui/", "audio/", "addons/", "network/", "auth/", "platform/")
# core/ headers that are infrastructure rather than client: header-only, or
# compiled into wowee_base.
CORE_ALLOWED = {
    "core/logger.hpp", "core/profiler.hpp", "core/env_flag.hpp", "core/thread_pool.hpp",
    "core/coordinates.hpp", "core/local_time.hpp", "core/data_paths.hpp",
    "core/memory_monitor.hpp", "core/geoset_rules.hpp",
    # Header-only: the struct a cache's owner reports its size in.
    "core/cache_stats.hpp",
    # Header-only, over thread_pool.hpp: the lifetime fence a map's work and
    # its terrain workers are tracked by. The renderer is handed a token; it
    # knows nothing of the client that owns the group.
    "core/job_group.hpp",
}

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def library_sources():
    cmake = (ROOT / "CMakeLists.txt").read_text(errors="ignore")
    block = re.search(r"set\(WOWEE_RENDER_CLIENT_SOURCES(.*?)\)", cmake, re.S)
    if not block:
        return None
    client = set(block.group(1).split())
    excluded = re.search(r'list\(FILTER WOWEE_RENDER_SOURCES EXCLUDE REGEX "\^src/rendering/\(([^)]*)\)/"\)',
                         cmake)
    excluded_dirs = excluded.group(1).split("|") if excluded else []
    sources = []
    for path in sorted((ROOT / "src" / "rendering").rglob("*.cpp")):
        rel = path.relative_to(ROOT).as_posix()
        if rel in client:
            continue
        if any(rel.startswith(f"src/rendering/{d}/") for d in excluded_dirs):
            continue
        sources.append(rel)
    return sources


def resolve(name, including):
    for base in (ROOT / "include", ROOT / "src", (ROOT / including).parent):
        candidate = base / name
        if candidate.is_file():
            return candidate.relative_to(ROOT).as_posix()
    return None


def main():
    sources = library_sources()
    if not sources:
        print("WOWEE_RENDER_CLIENT_SOURCES is gone from CMakeLists.txt, so there is nothing to check")
        return 1

    # Breadth first, remembering who reached each file, for the report.
    reached_from = {s: None for s in sources}
    queue = list(sources)
    offenders = []
    while queue:
        rel = queue.pop(0)
        text = (ROOT / rel).read_text(errors="ignore")
        for name in INCLUDE.findall(text):
            if name.startswith(FORBIDDEN) or (name.startswith("core/") and name not in CORE_ALLOWED):
                offenders.append((rel, name))
                continue
            target = resolve(name, rel)
            if target and target not in reached_from:
                reached_from[target] = rel
                queue.append(target)

    print(f"{len(sources)} wowee_render sources, {len(reached_from)} files reached through their includes\n")
    print(f"{len(offenders)} includes of the client from the renderer library:")
    for rel, name in offenders:
        chain = [rel]
        while reached_from.get(chain[-1]):
            chain.append(reached_from[chain[-1]])
        print(f"  {rel} includes {name}" + (f"  (via {' <- '.join(chain[1:])})" if len(chain) > 1 else ""))
    if not offenders:
        print("  (none)")
    return 1 if offenders else 0


if __name__ == "__main__":
    sys.exit(main())
