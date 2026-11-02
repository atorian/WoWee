#!/usr/bin/env python3
"""Two creatures of one model are each drawn in their own display's skin.

The client loads a creature model once for its file, and every display of
the file draws it: what makes a black wolf black and an arctic one white is
the display's skin, put on each instance (CharacterRenderer::setDisplaySkin),
not on the model. Written on the model instead, the last display dressed
every wolf; left off, every wolf is drawn untextured.

Two wolves of one model side by side, the left in WolfSkinBlack, the right
in WolfSkinArctic: the right must be drawn twice as light as the left.
"""
from pathlib import Path
import sys

from check_character_skinning import APART, HEIGHT, WIDTH, WOLF, render, scene

BLACK = r"Creature\Wolf\WolfSkinBlack.blp"
ARCTIC = r"Creature\Wolf\WolfSkinArctic.blp"
# The arctic wolf's mean brightness against the black one's, at least, under
# this scene's dim light: 45 against 16 in their skins; 46 against 45 with
# the last display's skin on both, 100 against 98 with neither's.
LIGHTER = 2.0


def wolves():
    return [{"path": WOLF, "offset": [-1.6, 0, 0], "skins": [[11, BLACK]]},
            {"path": WOLF, "offset": [1.6, 0, 0], "sameModelAs": 0, "skins": [[11, ARCTIC]]}]


def mean_brightness(pixels, mask):
    if not mask:
        return None
    return sum(sum(pixels[i * 3 + k] for k in range(3)) / 3 for i in mask) / len(mask)


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    source = scene(wolves())
    source["camera"] = {"offset": [0, -9, 1.2], "lookAt": [0, 0, 0.8]}
    empty_source = dict(source, characters=[])
    empty = render(binary, scratch, "empty", empty_source)
    drawn = render(binary, scratch, "skinned", source)
    pixels = range(WIDTH * HEIGHT)
    wolf = {i for i in pixels if max(abs(drawn[i * 3 + k] - empty[i * 3 + k]) for k in range(3)) > APART}
    left = {i for i in wolf if i % WIDTH < WIDTH // 2}
    right = {i for i in wolf if i % WIDTH >= WIDTH // 2}
    if len(left) < 200 or len(right) < 200:
        print(f"FAIL: the wolves are not both drawn ({len(left)} and {len(right)} pixels)")
        return 1
    dark, light = mean_brightness(drawn, left), mean_brightness(drawn, right)
    print(f"black wolf {dark:.1f}, arctic wolf {light:.1f} mean brightness ({len(left)} and {len(right)} pixels)")
    if light < LIGHTER * dark:
        print(f"FAIL: the arctic wolf is not {LIGHTER} times as light as the black one - "
              "one display's skin is on both, or neither has its own")
        return 1
    print("PASS: two creatures of one model are each in their own display's skin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
