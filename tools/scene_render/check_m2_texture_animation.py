#!/usr/bin/env python3
"""A doodad's texture animation moves its texture as the client moves it.

The view's draws sample a batch's texture animation on the GPU
(m2_draws.comp.glsl), at each instance's clock, into the records its
instances are drawn from. MODEL's brew is an opaque batch whose texture
moves; it is drawn at two moments of its animation, and each picture is held
against the one the CPU's sampling drew at the same moment before it moved to
the GPU (reference/texture_animation_*.png, from master at f898f437).

With the animation left out (WOWEE_M2_NO_TEXTURE_ANIMATION=1) the brew stands
still, which the reference does not: the run says so, or the scene could not
tell an animated texture from a still one. (The two moments cannot be set
against each other for that: the frame's exposure settles between them.)
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 240, 240
MODEL = r"World\Expansion02\Doodads\Generic\Scourge\SC_Cauldron.m2"
OFFSET = [0, 0, 0]
# Above the cauldron, looking down into its brew.
CAMERA = {"offset": [0, -5, 9], "lookAt": [0, 0, 1.5]}
MOMENTS = (12, 40)
# A pixel this far off in a channel differs.
APART = 8
# At most this share of pixels may differ from the reference: the GPU's
# arithmetic is not the CPU's to the last bit, which shows on texel edges.
MATCHES = 0.002


def scene(frames):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": [{"path": MODEL, "offset": OFFSET, "rotation": [0, 0, 0]}],
        "camera": CAMERA,
        "size": [WIDTH, HEIGHT], "frames": frames, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False, "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source, animated=True):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    # The texture alone: its particles are drawn at random.
    env["WOWEE_M2_NO_PARTICLES"] = "1"
    if animated:
        env.pop("WOWEE_M2_NO_TEXTURE_ANIMATION", None)
    else:
        env["WOWEE_M2_NO_TEXTURE_ANIMATION"] = "1"
    path = scratch / f"{name}.json"
    path.write_text(json.dumps(source), encoding="utf-8")
    png = scratch / f"{name}.png"
    run = subprocess.run([str(binary), str(path), str(png)], cwd=binary.parent, env=env, capture_output=True)
    output = run.stdout + run.stderr
    (scratch / f"{name}.log").write_bytes(output)
    if b"no Vulkan device" in output or b"no assets under" in output:
        raise SystemExit(77)
    if run.returncode:
        print(output.decode(errors="replace")[-4000:])
        raise SystemExit(1)
    if b"VUID-" in output or b"Validation Error:" in output:
        print("FAIL: Vulkan validation error; see", scratch / f"{name}.log")
        raise SystemExit(1)
    return rgb_pixels(png)


def differing(a, b):
    n = len(a) // 3
    return sum(1 for i in range(n) if max(abs(a[i * 3 + k] - b[i * 3 + k]) for k in range(3)) > APART) / n


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    reference = Path(__file__).resolve().parent / "reference"
    scratch.mkdir(parents=True, exist_ok=True)
    failures = []
    for frames in MOMENTS:
        drawn = render(binary, scratch, f"frames{frames}", scene(frames))
        expected = rgb_pixels(reference / f"texture_animation_{frames}.png")
        off = differing(drawn, expected)
        print(f"frame {frames}: {off:.4f} of the picture differs from the CPU's")
        if off > MATCHES:
            failures.append(f"at frame {frames} {off:.4f} of the picture differs from the CPU's (> {MATCHES})")
    still = render(binary, scratch, "still", scene(MOMENTS[1]), animated=False)
    off = differing(still, rgb_pixels(reference / f"texture_animation_{MOMENTS[1]}.png"))
    print(f"left still: {off:.4f} differs from the CPU's")
    if off <= MATCHES:
        failures.append(f"left still the picture still matches ({off:.4f}); the scene cannot see the animation")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: the texture moves as the CPU moved it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
