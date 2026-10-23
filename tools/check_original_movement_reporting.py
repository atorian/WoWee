#!/usr/bin/env python3
"""Compare send admission, packet bytes, reporting state and heartbeat timing."""
import argparse
import json
import random
import subprocess
from pathlib import Path
from check_original_movement_ack import cases as ack_cases, probe_input as ack_input
from check_original_collision import f32

def cases():
    rng = random.Random(0x71f0c0)
    states = list(ack_cases())
    ordinary = [0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
                0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc9, 0xca, 0xcb, 0xd9, 0xda,
                0xdb, 0xee, 0x2d1, 0x346, 0x359, 0x35a, 0x3a7]
    for i in range(2560):
        c = dict(states[(i // 2) % len(states)])
        if i % 2:
            c['opcode'] = ordinary[(i // 2) % len(ordinary)]
        c.update(name=f'report_{i}', active=bool((i // 40) % 2),
                 flags=c['flags'] | rng.choice([0, 0x10, 0x20, 0x40, 0x80, 0xf0]),
                 time=rng.choice([0, 1, 0xffffff00, 0xffffffff, rng.getrandbits(32)]),
                 unit_flags=rng.getrandbits(32), heartbeat_time=rng.getrandbits(32),
                 reported_facing=f32(rng.uniform(-7, 7)), reported_pitch=f32(rng.uniform(-2, 2)))
        yield c

def probe_input(c):
    return ack_input(c).rstrip() + ' ' + ' '.join(map(str, [int(c['active']), c['unit_flags'],
        c['heartbeat_time'], c['reported_facing'], c['reported_pitch']])) + '\n'

def main(cases=cases, probe_input=probe_input,
         reference_module='original_movement_reporting_reference',
         reference_class='OriginalMovementReporting', probe_args=(),
         routines='SendMovementUpdate 0x71f0c0, PrepareMovementPacket 0x71ef80, admission 0x71ef20, UpdateHeartbeatTimerA 0x6e9b70; unattached, no active spline; real packet writers; supplied getters, storage, globals and network capture'):
    p = argparse.ArgumentParser(description=__doc__)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument('--binary'); src.add_argument('--reference')
    p.add_argument('--probe'); p.add_argument('--opcodes', default='Data/expansions/wotlk/opcodes.json')
    p.add_argument('--write-reference'); p.add_argument('--failures')
    a = p.parse_args()
    if a.reference:
        records = json.loads(Path(a.reference).read_text())['cases']
    else:
        from importlib import import_module
        from original_collision_reference import REFERENCE_SHA256
        original = getattr(import_module(reference_module), reference_class)(a.binary)
        records = [dict(input=c, output=original.run(c)) for c in cases()]
        if a.write_reference:
            header = json.dumps(dict(binary_sha256=REFERENCE_SHA256,
                routines=routines,
                fpu_control=0x27f), indent=2)
            Path(a.write_reference).write_text(header[:-2] + ',\n  "cases": [\n' +
                ',\n'.join('    ' + json.dumps(r) for r in records) + '\n  ]\n}\n')
    if not a.probe:
        print(f'Generated {len(records)} original movement reports')
        return 0
    run = subprocess.run([a.probe, a.opcodes, *probe_args], input=''.join(probe_input(r['input']) for r in records),
                         capture_output=True, text=True, timeout=90)
    if run.returncode:
        raise RuntimeError(run.stderr)
    actual = [json.loads(line) for line in run.stdout.splitlines()]
    if len(actual) != len(records):
        raise RuntimeError('Missing reporting results')
    failures = [dict(input=r['input'], expected=r['output'], actual=g)
                for r, g in zip(records, actual) if r['output'] != g]
    if a.failures:
        Path(a.failures).write_text(json.dumps(failures, indent=2))
    print(json.dumps(dict(cases=len(records), failures=len(failures), details=failures[:1]), indent=2))
    return bool(failures)
if __name__ == '__main__':
    raise SystemExit(main())
