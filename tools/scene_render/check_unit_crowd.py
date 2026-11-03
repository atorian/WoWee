#!/usr/bin/env python3
"""A unit in a crowd is drawn as it is drawn among any other crowd.

The cull on the GPU (char_draws.comp.glsl) runs a thread a batch of every
unit, so one wave holds batches of several units - and their animation blobs
lie on whichever pages of the skeleton pool they were placed in. A page read
by an index the threads of a wave do not share must say so (nonuniformEXT);
unmarked, AMD read one thread's page for the whole wave, and units sampled
another model's colour and weight tracks. Whole bodies and mounts went
missing in Dalaran, where they stood beside others, and came back as the
camera turned and the cull left other threads out of the wave.

Seventy-nine creatures stand on a grid, loaded first to last and then last
to first: the same places, other neighbours in each wave and other pages for
their blobs. The cull makes as many draws either way, and the two pictures
are one picture but for the particles, whose randomness follows the order
their emitters were placed in.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from check_sharp_stars import rgb_pixels

WIDTH, HEIGHT = 960, 720
# A pixel this far off in a channel differs.
APART = 8
# The particles' share of the frame that may differ between the orders: 269
# pixels of 691,200 here, against 36,956 with the page read uniformly.
MOST_APART = 2000

CREATURES = [
    r"creature\airelemental\airelemental.m2",
    r"creature\alliancebrasscannon\alliancebrasscannon.m2",
    r"creature\ancientofwar2diseased\ancientofwar2diseased.m2",
    r"creature\anubisath\anubisath.m2",
    r"creature\arcticcondor\arcticcondor.m2",
    r"creature\babybeholder\babybeholder.m2",
    r"creature\bainebloodhoof\bainebloodhoof.m2",
    r"creature\beargod\beargod.m2",
    r"creature\beholder\beholder.m2",
    r"creature\bloodgod\bloodgod.m2",
    r"creature\bogbeast\bogbeast.m2",
    r"creature\botani\botani.m2",
    r"creature\cat\cat.m2",
    r"creature\chickenmount\chickenmount.m2",
    r"creature\clefthoove\clefthoove.m2",
    r"creature\coldwraith\coldwraith.m2",
    r"creature\crab\crab.m2",
    r"creature\cryptfiend\cryptfiend.m2",
    r"creature\cupid\cupid.m2",
    r"creature\demolishercannonball\demolishercannonball.m2",
    r"creature\diemetradon\diemetradon.m2",
    r"creature\doomguardoutland\doomguardoutland.m2",
    r"creature\dragon\dragon.m2",
    r"creature\dragonhawkgod\dragonhawkgod.m2",
    r"creature\dragonspawncaster\dragonspawncaster.m2",
    r"creature\dragonwhelpdarkshade\dragonwhelpdarkshade.m2",
    r"creature\drake\drake.m2",
    r"creature\druidaquatickultiran\druidaquatickultiran.m2",
    r"creature\druidbearzandalaritroll\druidbearzandalaritroll.m2",
    r"creature\druidcatzandalaritroll\druidcatzandalaritroll.m2",
    r"creature\druidowlbear2\druidowlbear2.m2",
    r"creature\druidtravelkultiran\druidtravelkultiran.m2",
    r"creature\dwarfmalewarriorlight\dwarfmalewarriorlight.m2",
    r"creature\ent2\ent2.m2",
    r"creature\eredarfemale\eredarfemale.m2",
    r"creature\facelessgeneral\facelessgeneral.m2",
    r"creature\felbeast\felbeast.m2",
    r"creature\felelfcasterfemale\felelfcasterfemale.m2",
    r"creature\felguard\felguard.m2",
    r"creature\felorcnetherdrake\felorcnetherdrake.m2",
    r"creature\fireelemental\fireelemental.m2",
    r"creature\fleshbeast\fleshbeast.m2",
    r"creature\flyingbomber\flyingbomber.m2",
    r"creature\flyingreindeer\flyingreindeer.m2",
    r"creature\freia\freia.m2",
    r"creature\frostnymph\frostnymph.m2",
    r"creature\frostwurmnorthrend\frostwurmnorthrend.m2",
    r"creature\furbolg2\furbolg2.m2",
    r"creature\geist\geist.m2",
    r"creature\ghoul\ghoul.m2",
    r"creature\gnollcaster\gnollcaster.m2",
    r"creature\gnomespidertank\gnomespidertank.m2",
    r"creature\golemharvest\golemharvest.m2",
    r"creature\gorilla\gorilla.m2",
    r"creature\gryphonpet\gryphonpet.m2",
    r"creature\harkoa\harkoa.m2",
    r"creature\hhgroundmount\hhgroundmount.m2",
    r"creature\hogger\hogger.m2",
    r"creature\horisath\horisath.m2",
    r"creature\hufmmerchant\hufmmerchant.m2",
    r"creature\humanfemalemerchantfat\humanfemalemerchantfat.m2",
    r"creature\humanfemalewarriormedium\humanfemalewarriormedium.m2",
    r"creature\humanmalekid\humanmalekid.m2",
    r"creature\humanmalepiratecrewman\humanmalepiratecrewman.m2",
    r"creature\humanmalewizard\humanmalewizard.m2",
    r"creature\humlmerchant\humlmerchant.m2",
    r"creature\humsmerchant\humsmerchant.m2",
    r"creature\icecrownfleshbeast\icecrownfleshbeast.m2",
    r"creature\inferna1\inferna1.m2",
    r"creature\irondwarf\irondwarf.m2",
    r"creature\jormungarlarva\jormungarlarva.m2",
    r"creature\kelthuzad\kelthuzad.m2",
    r"creature\kingymiron\kingymiron.m2",
    r"creature\krakken\krakken.m2",
    r"creature\larva2\larva2.m2",
    r"creature\lavaman\lavaman.m2",
    r"creature\lion\lion.m2",
    r"creature\lostone\lostone.m2",
    r"creature\makrura2\makrura2.m2",
]


def scene(characters):
    return {
        "map": "Northrend", "mapId": 571, "zoneId": 4395, "hour": 12.0,
        "origin": [5818, 405, 650], "streamWorld": False, "models": [], "characters": characters,
        "camera": {"offset": [0, -70, 70], "lookAt": [0, 0, 0]},
        "size": [WIDTH, HEIGHT], "frames": 12, "rayTracedLighting": 0,
        "volumetricFog": 0, "shadowQuality": 0, "msaa": 1, "bloom": 0,
        "clouds": False, "sunShafts": False, "wind": {"direction": 0, "speed": 0},
    }


def render(binary, scratch, name, source):
    env = os.environ.copy()
    env["WOWEE_CONFIG_ROOT"] = str(scratch / "config")
    env["WOWEE_RESOURCE_ROOT"] = str(binary.parent)
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
    counts = re.search(rb"character draws in the kept frame: (\d+) solid, (\d+) faded", output)
    if not counts:
        print("FAIL: scene_render said nothing of the character draws; see", scratch / f"{name}.log")
        raise SystemExit(1)
    return rgb_pixels(png), int(counts.group(1)) + int(counts.group(2))


def main() -> int:
    binary, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    grid = [{"path": path, "offset": [-45 + 10 * (i % 10), -35 + 10 * (i // 10), 0]}
            for i, path in enumerate(CREATURES)]
    forward, forward_draws = render(binary, scratch, "forward", scene(grid))
    reverse, reverse_draws = render(binary, scratch, "reverse", scene(list(reversed(grid))))
    apart = sum(1 for i in range(WIDTH * HEIGHT)
                if max(abs(forward[i * 3 + k] - reverse[i * 3 + k]) for k in range(3)) > APART)
    print(f"draws: {forward_draws} loaded first to last, {reverse_draws} last to first; "
          f"{apart} of {WIDTH * HEIGHT} pixels apart")
    failed = False
    if forward_draws != reverse_draws:
        print("FAIL: the cull drew another number of batches when the units were loaded in another order")
        failed = True
    if apart > MOST_APART:
        print(f"FAIL: more than {MOST_APART} pixels differ between the two orders")
        failed = True
    if failed:
        return 1
    print("PASS: every unit of the crowd is drawn as it is whatever its neighbours")
    return 0


if __name__ == "__main__":
    sys.exit(main())
