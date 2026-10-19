#!/usr/bin/env python3
"""The collision world reaching above itself.

    tools/collision_dependency_check.py

WHY

wowee_collision is the solid world of a map - terrain heights, building and
doodad shapes and where they stand - and every floor, wall and sight query
over it. The renderers place what they draw in it and the client and the game
ask it; it is what lets the renderer stop answering for the world, and later
draw on a thread of its own. That holds only while the collision world knows
nothing of the renderer, the game or the interface: an include of one of them
compiles without complaint, because every target has every include path.

WHAT IT DOES

Follows the quoted includes of src/collision and include/collision through
include/ and src/, and reports every file reached outside collision/,
pipeline/ and the header-only core infrastructure wowee_base carries.

WHAT IT CANNOT SEE

A dependency that is not an include: a symbol declared by hand. The link of
test_collision_world, which links wowee_collision alone, would catch that.
"""
import sys

# The same reading of includes as the renderer's own check.
from render_dependency_check import INCLUDE, ROOT, resolve

ALLOWED_PREFIXES = ("collision/", "pipeline/")
CORE_ALLOWED = {"core/logger.hpp", "core/coordinates.hpp", "core/profiler.hpp"}


def main():
    sources = sorted(p.relative_to(ROOT).as_posix()
                     for top in ("src/collision", "include/collision")
                     for p in (ROOT / top).rglob("*") if p.suffix in (".cpp", ".hpp", ".h"))
    if not sources:
        print("src/collision and include/collision are gone, so there is nothing to check")
        return 1

    reached_from = {s: None for s in sources}
    queue = list(sources)
    offenders = []
    while queue:
        rel = queue.pop(0)
        text = (ROOT / rel).read_text(errors="ignore")
        for name in INCLUDE.findall(text):
            allowed = name.startswith(ALLOWED_PREFIXES) or name in CORE_ALLOWED
            if not allowed:
                offenders.append((rel, name))
                continue
            target = resolve(name, rel)
            if target and target not in reached_from:
                reached_from[target] = rel
                queue.append(target)

    print(f"{len(sources)} collision files, {len(reached_from)} files reached through their includes\n")
    print(f"{len(offenders)} includes from the collision world above itself:")
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
