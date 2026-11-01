#!/usr/bin/env python3
"""Compare shared movement state changes with original build 12340 execution."""
import argparse, json, random, subprocess
from pathlib import Path
from check_original_collision import f32, vector
from check_original_surface import matches
from check_original_world_movement import apply_cases, probe_input as frame_input

def probe_input(c):
    return frame_input(c).rstrip() + ' ' + str(c['command']) + ' ' + str(c['value']) + '\n'

def cases():
    rng = random.Random(0x98a0b0)
    base = next(apply_cases())
    for i in range(5120):
        command = i % 16 if i < 4096 else (16 if i < 4608 else 18) + i % 2
        yield dict(base, name=f'state_{i}', command=command,
            value=f32(rng.uniform(-10, 50)) if command < 9 else i // 16 % 2,
            flags=rng.getrandbits(32) & ~0x08000000, flags2=rng.getrandbits(16),
            position=vector([rng.uniform(-100, 100) for _ in range(3)]),
            facing=f32(rng.uniform(-7, 7)), pitch=f32(rng.uniform(-1.5, 1.5)),
            current_speed=f32(rng.uniform(0, 20)), anchor_elapsed=rng.getrandbits(32),
            fall_time=rng.choice([0, 1, 50, 1000, rng.getrandbits(32)]),
            fall_velocity=f32(rng.uniform(-20, 100)),
            speeds=vector([rng.uniform(0, 20) for _ in range(7)]),
            turn_rate=f32(rng.uniform(0, 10)), pitch_rate=f32(rng.uniform(0, 10)))
    modes = [0, 1, 0x1000, 0x1001, 0x200001, 0x2000001, 0x800, 0x100000]
    for command in range(20):
        values = [0, 2**-24, -(2**-24), 2**-22, -(2**-22), 2**-21, 1, -1] if command < 9 else [0, 1]
        for flags in modes:
            for index, value in enumerate(values):
                yield dict(base, name=f'state_boundary_{command}_{flags}_{index}',
                    command=command, value=value, flags=flags, flags2=0,
                    speeds=[0] * 7, turn_rate=0, pitch_rate=0,
                    position=[2, 3, 4], facing=f32(.7), pitch=f32(.3),
                    fall_time=1234, fall_velocity=-7, anchor_elapsed=333)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument('--binary'); src.add_argument('--reference')
    p.add_argument('--probe'); p.add_argument('--write-reference'); p.add_argument('--failures')
    a = p.parse_args()
    if a.reference:
        records = json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_state_reference import OriginalMovementState
        from original_collision_reference import REFERENCE_SHA256
        original = OriginalMovementState(a.binary)
        records = [dict(input=c, output=original.run(c)) for c in cases()]
        if a.write_reference:
            header = json.dumps(dict(binary_sha256=REFERENCE_SHA256, fpu_control=0x27f,
                routines='CMovementShared speed/rate changes, gravity, feather fall/post-fall, water walk, hover, root/unroot; no spline'), indent=2)
            Path(a.write_reference).write_text(header[:-2] + ',\n  "cases": [\n' +
                ',\n'.join('    ' + json.dumps(r) for r in records) + '\n  ]\n}\n')
    if not a.probe:
        print(f'Generated {len(records)} original state transitions')
        return 0
    run = subprocess.run([a.probe, '--world-state'], input=''.join(probe_input(r['input']) for r in records),
                         capture_output=True, text=True, timeout=90)
    if run.returncode: raise RuntimeError(run.stderr)
    actual = [json.loads(line) for line in run.stdout.splitlines()]
    if len(actual) != len(records): raise RuntimeError('Missing state transition results')
    failures = [dict(input=r['input'], expected=r['output'], actual=g)
                for r, g in zip(records, actual) if not matches(r['output'], g)]
    if a.failures: Path(a.failures).write_text(json.dumps(failures, indent=2))
    print(json.dumps(dict(cases=len(records), failures=len(failures), details=failures[:1]), indent=2))
    return bool(failures)
if __name__ == '__main__': raise SystemExit(main())
