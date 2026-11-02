#!/usr/bin/env python3
"""M2 particles and ribbons are drawn where the client draws them.

Particles and ribbons run on the GPU (m2_particles.comp, m2_ribbons.comp):
each emitter's tracks sampled at its instance's time, standing on its bone
as posed this frame; particles born there, moved under gravity, drag and
wind and laid out as quads; ribbon edges laid from there along the bone's
path and drawn as strips. Three fires, each seen after a second of
burning:

- a bonfire doodad, whose flames rise from its logs (OrcBonFire.m2);
- a fire elemental, a creature (CharacterRenderer poses it), whose flames
  stand on its body;
- a phoenix, whose flames ride its beating wings, and its wing trails
  alone (ribbons).

What is drawn is where the frame differs from the same frame without it
(WOWEE_M2_NO_PARTICLES=1, WOWEE_M2_NO_RIBBONS=1). Particles are random, so
they are not compared a pixel at a time: the cover of each, widened by a
few pixels, must share at least SHARED of itself with what the client drew
while its skeletons, particles and ribbons still ran on the CPU (master's
build of 2 November 2026, before stage 3.8; written by this script with
--write-reference into reference/particles_<name>.png, white where it
was). The GPU's share is 0.72, 0.86 and 0.59; a phoenix whose flames stand
on its body's root rather than its wings', 0.32; particles born without
their emitter's offset on its bone, 0.51, 0.31 and 0.25. Ribbons are not
random, and share more (RIBBONS_SHARED): 0.997, and trails laid from the
root bone 0.10. And a scene runs the same every time: drawn twice, it is
the same pixels.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 320, 320
# A pixel this far off in a channel is a particle's.
APART = 24
# How far a cover is widened before the two are compared, in pixels.
WIDEN = 4
SHARED = 0.5
RIBBONS_SHARED = 0.9
REFERENCE = Path(__file__).resolve().parent / "reference"

CASES = {
    "bonfire": {
        "models": [{"path": r"World\Azeroth\BurningSteppes\PassiveDoodads\Bonfire\OrcBonFire.m2",
                    "offset": [0, 0, 0.5]}],
        "characters": [],
        "camera": {"offset": [0, -9, 3], "lookAt": [0, 0, 2]},
    },
    "elemental": {
        "models": [],
        "characters": [{"path": r"Creature\FireElemental\FireElemental.m2", "offset": [0, 0, 0.5]}],
        "camera": {"offset": [0, -9, 3], "lookAt": [0, 0, 2]},
    },
    "phoenix": {
        "models": [],
        "characters": [{"path": r"Creature\Phoenix\Phoenix.m2", "offset": [0, 0, 1]}],
        "camera": {"offset": [0, -12, 3], "lookAt": [0, 0, 3]},
    },
    # Its wing trails alone: ribbons, laid from the wings' bones as they beat.
    "phoenix_ribbons": {
        "models": [],
        "characters": [{"path": r"Creature\Phoenix\Phoenix.m2", "offset": [0, 0, 1]}],
        "camera": {"offset": [0, -12, 3], "lookAt": [0, 0, 3]},
        "drawn": {"WOWEE_M2_NO_PARTICLES": "1"},
        "without": {"WOWEE_M2_NO_PARTICLES": "1", "WOWEE_M2_NO_RIBBONS": "1"},
        "shared": RIBBONS_SHARED,
    },
}


def scene(case):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False,
        "models": case["models"], "characters": case["characters"], "camera": case["camera"],
        "size": [WIDTH, HEIGHT], "frames": 60, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False, "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source, switches):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env["WOWEE_SINGLE_THREAD_RECORD"] = "1"
    for switch in ("WOWEE_M2_NO_PARTICLES", "WOWEE_M2_NO_RIBBONS"):
        env.pop(switch, None)
    env.update(switches)
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


def covered(frame, without):
    return {i for i in range(WIDTH * HEIGHT)
            if max(abs(frame[i * 3 + k] - without[i * 3 + k]) for k in range(3)) > APART}


def widened(pixels):
    out = set()
    for i in pixels:
        x, y = i % WIDTH, i // WIDTH
        for dy in range(-WIDEN, WIDEN + 1):
            for dx in range(-WIDEN, WIDEN + 1):
                if 0 <= x + dx < WIDTH and 0 <= y + dy < HEIGHT:
                    out.add((y + dy) * WIDTH + x + dx)
    return out


def write_mask(path, pixels):
    raw = bytearray()
    for y in range(HEIGHT):
        raw.append(0)
        for x in range(WIDTH):
            raw.extend(b"\xff\xff\xff" if y * WIDTH + x in pixels else b"\x00\x00\x00")

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    write_reference = "--write-reference" in sys.argv[3:]
    scratch.mkdir(parents=True, exist_ok=True)
    failures = []
    for name, case in CASES.items():
        source = scene(case)
        switches = case.get("drawn", {})
        drawn = render(binary, scratch, name, source, switches)
        without = render(binary, scratch, f"{name}-without", source,
                         case.get("without", {"WOWEE_M2_NO_PARTICLES": "1"}))
        pixels = covered(drawn, without)
        if write_reference:
            write_mask(REFERENCE / f"particles_{name}.png", pixels)
            print(f"{name}: {len(pixels)} pixels written")
            continue
        if len(pixels) < 200:
            failures.append(f"{name}: particles cover {len(pixels)} pixels; they are not drawn")
            continue
        reference = rgb_pixels(REFERENCE / f"particles_{name}.png")
        expected = {i for i in range(WIDTH * HEIGHT) if reference[i * 3] > 127}
        a, b = widened(pixels), widened(expected)
        shared = len(a & b) / len(a | b)
        print(f"{name}: {len(pixels)} pixels, the client's {len(expected)}; {shared:.3f} shared")
        least = case.get("shared", SHARED)
        if shared < least:
            failures.append(f"{name}: {shared:.3f} shared with the client's (< {least}) - "
                            "what its emitters draw is not where they are")
        if render(binary, scratch, f"{name}-again", source, switches) != drawn:
            failures.append(f"{name}: drawn twice, the particles differ - a scene does not run the same")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    if not write_reference:
        print("PASS: particles emitted, flown and drawn where the client drew them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
