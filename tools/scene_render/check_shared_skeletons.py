#!/usr/bin/env python3
"""A creature loaded after many others of one model is still posed.

The client loads a unit's model once per creature display, and the displays
that dress one race's model differently - every humanoid NPC - are each a
model of their own. Their skeletons, every key of every sequence, are the
same words: a troll's are a million of them, four megabytes. Held apart,
Dalaran's displays took all 64 pages of the skeleton pool and every model
loaded after that stood in its bind pose - the player's own among them,
after logging out and back in.

Two hundred trolls, each loaded as a display of its own and stood behind
the camera, then a wolf in view: the wolf must stand in its stand's pose,
the silhouette check_character_skinning.py holds it to (reference
wolf_stand_silhouette.png). Two hundred separate troll skeletons are more
than the pool's 64 pages hold; one shared skeleton is a quarter of a page.
"""
import os
from pathlib import Path
import sys

from check_character_skinning import APART, REFERENCE, SHARED, WIDTH, HEIGHT, WOLF, render, scene
from check_sharp_stars import rgb_pixels

TROLL = r"Character\Troll\Male\TrollMale.m2"
TROLLS = 200


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    behind = [{"path": TROLL, "offset": [0, -40, 0]} for _ in range(TROLLS)]
    empty = render(binary, scratch, "empty", scene(behind))
    posed = render(binary, scratch, "posed", scene(behind + [{"path": WOLF}]))
    log = (scratch / "posed.log").read_bytes()
    if b"Skeleton pool: all" in log:
        print("FAIL: the skeleton pool ran out of pages:")
        for line in log.splitlines():
            if b"Skeleton pool" in line:
                print("  ", line.decode(errors="replace"))
        return 1
    reference = rgb_pixels(REFERENCE)
    pixels = range(WIDTH * HEIGHT)
    drawn = {i for i in pixels if max(abs(posed[i * 3 + k] - empty[i * 3 + k]) for k in range(3)) > APART}
    expected = {i for i in pixels if reference[i * 3] > 127}
    if len(drawn) < 200:
        print(f"FAIL: the wolf is not drawn ({len(drawn)} pixels)")
        return 1
    shared = len(drawn & expected) / len(drawn | expected)
    print(f"wolf after {TROLLS} trolls: {len(drawn)} pixels, the posed one {len(expected)}; {shared:.3f} shared")
    if shared < SHARED:
        print(f"FAIL: under {SHARED} shared - the wolf loaded after the trolls is not posed")
        return 1
    print("PASS: a creature loaded after two hundred of one model is posed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
