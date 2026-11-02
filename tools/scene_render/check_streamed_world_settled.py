#!/usr/bin/env python3
"""A scene draws the same frame however long the world takes to stream in.

The map's distant terrain (its WDL) loads on a job of its own whenever the
world streams, and is drawn from the frame it is resident. Dalaran's
lamppost is drawn twice: as it comes, and with that load held back three
seconds (WOWEE_DISTANT_TERRAIN_DELAY_MS), as a slow disk would. The two
pictures are the same byte for byte: the kept frame does not depend on how
far the load had got.

They also differ from the same scene with the world not streamed at all
(`"streamWorld": false`), so the distant terrain is in the picture being
compared, not missing from both.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

from check_sharp_stars import rgb_pixels

SCENE = Path(__file__).resolve().parent / "scenes" / "lamppost.json"
HELD_BACK_MS = 3000


def load_scene():
    text = "\n".join(line for line in SCENE.read_text(encoding="utf-8").splitlines()
                     if not line.lstrip().startswith("//"))
    return json.loads(text)


def render(binary, scratch, name, source, delay_ms=None):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
    env.pop("WOWEE_DISTANT_TERRAIN_DELAY_MS", None)
    if delay_ms is not None:
        env["WOWEE_DISTANT_TERRAIN_DELAY_MS"] = str(delay_ms)
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


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    scene = load_scene()

    prompt = render(binary, scratch, "as-it-comes", scene)
    slow = render(binary, scratch, "held-back", scene, delay_ms=HELD_BACK_MS)
    unstreamed = render(binary, scratch, "not-streamed", dict(scene, streamWorld=False))

    failures = []
    if prompt == unstreamed:
        failures.append("the streamed world is not in the picture: it is the same as with nothing streamed")
    if slow != prompt:
        changed = sum(1 for i in range(0, len(prompt), 3) if prompt[i:i + 3] != slow[i:i + 3])
        failures.append(f"{changed} pixels differ when the distant terrain loads "
                        f"{HELD_BACK_MS} ms later; the kept frame depends on load speed")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: the kept frame is the same however long the world takes to stream in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
