#!/usr/bin/env python3
"""Colour has to survive the mip chain the upscaler builds.

    tools/upscale_mip_check.py

Pillow's resize weights colour by alpha. A texel that is fully transparent
contributes nothing to the average and comes back black, so an RGBA resize
quietly throws away whatever is painted under the transparent part of a sheet.

That is harmless while every texture carrying alpha is a cutout - nothing
samples a texel it has keyed away. It stopped being harmless when an M2 batch
marked opaque stopped keying: blend mode 0 does not read alpha at all, and
Silverpine's trunks sample an atlas panel whose alpha is left over from another
layer. Level 0 held the bark and every level under it was black, so the trunks
were solid up close and holed in black at any distance. 727 of the 823
alpha-bearing sidecars were built that way.

So the tool halves the channels separately, and this holds it there. The trap
is silent and one line wide - build_mip_chain going back to Image.resize on an
RGBA image would reintroduce it with nothing to show for it until someone
walked a mip chain by hand.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:                   # pragma: no cover - environment
    print(f"skipped: {exc}")
    sys.exit(0)

from upscale_textures import (alpha_coverage, backing_is_painted, build_mip_chain,
                              halve_straight, restore_coverage, restore_hidden_colour)

BARK = (107, 84, 47)          # measured under the alpha of SilverPineTree01TrunkSkin
CUTOFF = 0.5
DARK = 20                     # luma below which a backing is a backing


def luma(rgb):
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def sheet():
    """Bright colour everywhere, transparent down the left half."""
    arr = np.zeros((64, 64, 4), np.uint8)
    arr[..., 0], arr[..., 1], arr[..., 2] = BARK
    arr[:, :32, 3] = 0
    arr[:, 32:, 3] = 255
    return arr


def colour_under_alpha(level):
    clear = level[..., 3] < 50
    if not clear.any():
        return None
    return level[..., :3][clear].mean(axis=0)


def main() -> int:
    failures = []

    # The premultiply itself, named so the reason this file exists stays
    # visible even if Pillow ever changes its mind.
    resized = np.asarray(Image.fromarray(sheet(), "RGBA").resize((32, 32), Image.BOX))
    if luma(colour_under_alpha(resized)) >= DARK:
        print("note: Pillow's RGBA resize no longer premultiplies")

    halved = halve_straight(sheet())
    under = colour_under_alpha(halved)
    if under is None or luma(under) < DARK:
        failures.append(f"halve_straight lost the colour under alpha: {under}")

    levels = build_mip_chain(sheet(), CUTOFF)
    if len(levels) < 6:
        failures.append(f"mip chain stopped at {len(levels)} levels")
    for i, level in enumerate(levels):
        under = colour_under_alpha(level)
        if under is None:
            continue
        if luma(under) < DARK:
            failures.append(
                f"mip {i} ({level.shape[1]}x{level.shape[0]}) went black under alpha: "
                f"{under.round(1)}, luma {luma(under):.1f}")

    # Holding coverage must not take opacity away. The restore used to scale
    # the whole channel, and a sheet of 0 and 255 keeps its coverage under any
    # factor from a half up: it settled low, and Dalaran's wagon roof came
    # back at 194, under the 224 a keyed material is cut at.
    for i, level in enumerate(levels):
        # Down to the level where both halves still have a texel of their own.
        if level.shape[1] >= 2 and level[..., 3].max() != 255:
            failures.append(f"mip {i} lost its opaque texels: alpha tops out at "
                            f"{level[..., 3].max()}")
    soft = np.asarray(Image.fromarray(sheet()).resize((256, 256), Image.LANCZOS)).copy()
    target = alpha_coverage(sheet()[..., 3], CUTOFF)
    held = restore_coverage(soft, target, CUTOFF)
    if held[..., 3].max() != 255 or held[..., 3].min() != 0:
        failures.append(f"restore_coverage moved the ends of the alpha range: "
                        f"{held[..., 3].min()}..{held[..., 3].max()}")
    if abs(alpha_coverage(held[..., 3], CUTOFF) - target) > 1.0 / 256:
        failures.append(f"restore_coverage held {alpha_coverage(held[..., 3], CUTOFF):.4f}, "
                        f"not {target:.4f}")
    # Too fat by a column of edge: it has to go under the cutout, opaque
    # staying opaque. (Texels of one alpha cannot be split by any remap, so the
    # extra column is a ramp value, as a resample leaves it.)
    fat = sheet()
    fat[:, 31, 3] = 200
    thinned = restore_coverage(fat, target, CUTOFF)
    if abs(alpha_coverage(thinned[..., 3], CUTOFF) - target) > 1e-6 or \
            thinned[:, 32:, 3].min() != 255:
        failures.append("restore_coverage could not thin a sheet without dimming it")
    if not (restore_coverage(sheet(), target, CUTOFF) == sheet()).all():
        failures.append("restore_coverage changed a sheet whose coverage already held")

    # A painted backing is artwork an opaque batch samples; a black one is the
    # compressor's junk behind a silhouette. Dilating the first destroys it,
    # and the client draws exactly the first kind opaque, so the two sides of
    # the pipeline have to agree on which is which.
    if not backing_is_painted(sheet()):
        failures.append("backing_is_painted called painted bark a silhouette")
    black = sheet()
    black[:, :32, :3] = (7, 6, 4)          # Alterac's thorn cards, measured
    if backing_is_painted(black):
        failures.append("backing_is_painted called a black backing artwork")
    sparse = sheet()
    sparse[:, :32, 3] = 255
    sparse[:2, :4, 3] = 0                  # 8 texels, below the floor
    if backing_is_painted(sparse):
        failures.append("backing_is_painted drew a verdict from 8 texels")

    # The restore puts the source back under the alpha and leaves the rest of
    # the upscale alone.
    upscaled = np.zeros((128, 128, 4), np.uint8)
    upscaled[..., :3] = (255, 0, 255)      # stand-in for what a model invented
    upscaled[:, :64, 3] = 0
    upscaled[:, 64:, 3] = 255
    healed = restore_hidden_colour(upscaled, sheet())
    under = colour_under_alpha(healed)
    if under is None or abs(under[0] - BARK[0]) > 8:
        failures.append(f"restore_hidden_colour did not put the source back: {under}")
    if not (healed[:, 64:, :3] == (255, 0, 255)).all():
        failures.append("restore_hidden_colour overwrote the opaque texels")

    for line in failures:
        print(f"FAIL: {line}")
    if failures:
        return 1
    print(f"ok: colour holds under alpha across {len(levels)} mip levels, "
          "coverage held without dimming, backing classified, source restored")
    return 0


if __name__ == "__main__":
    sys.exit(main())
