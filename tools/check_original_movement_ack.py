#!/usr/bin/env python3
"""Compare full forced movement ACK packets with original client execution."""
import argparse, json, random, subprocess
from pathlib import Path
from check_original_movement_status import cases as status_cases
from check_original_collision import f32

def cases():
    rng = random.Random(0x71ef80)
    # Ordinary forced ACKs. Teleport/worldport use different packet layouts.
    opcodes = [0xe3, 0xe5, 0xe7, 0x2db, 0x2dd, 0x2df, 0x382, 0x384, 0x45d,
               0xf6, 0x2cf, 0x2d0, 0x340, 0x345, 0x517, 0xe9, 0xeb, 0xf0, 0x4cf, 0x4d1]
    states = list(status_cases())
    for i in range(1280):
        c = dict(states[(i // 20 * 17) % len(states)])
        c.update(name=f'force_ack_{i}', opcode=opcodes[i % 20],
                 guid=rng.choice([1, 0x1020000000300040, rng.getrandbits(64)]),
                 counter=rng.getrandbits(32), value=f32(rng.choice([0, 1, rng.uniform(-20, 100)])))
        yield c

def probe_input(c):
    values = [c['opcode'], c['guid'], c['counter'], c['value'], c['flags'], c['flags2'], c['time'],
              *c['position'], c['facing'], c['pitch'], c['fall_time'], c['fall_velocity'],
              *c['move_direction_2d'], c['current_speed'], c['step_start']]
    return c['name'] + ' ' + ' '.join(map(str, values)) + '\n'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument('--binary'); src.add_argument('--reference')
    p.add_argument('--probe'); p.add_argument('--opcodes', default='Data/expansions/wotlk/opcodes.json')
    p.add_argument('--write-reference'); p.add_argument('--failures')
    a = p.parse_args()
    if a.reference:
        records = json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_ack_reference import OriginalMovementAck
        from original_collision_reference import REFERENCE_SHA256
        original = OriginalMovementAck(a.binary)
        records = [dict(input=c, output=original.run(c)) for c in cases()]
        if a.write_reference:
            header = json.dumps(dict(binary_sha256=REFERENCE_SHA256,
                routines='PrepareMovementPacket 0x71ef80; original admission, GetMoveStatus and packet writers; active unattached mover, no spline',
                fpu_control=0x27f), indent=2)
            Path(a.write_reference).write_text(header[:-2] + ',\n  \"cases\": [\n' +
                ',\n'.join('    ' + json.dumps(r) for r in records) + '\n  ]\n}\n')
    if not a.probe:
        print(f'Generated {len(records)} original ACK packets')
        return 0
    run = subprocess.run([a.probe, a.opcodes], input=''.join(probe_input(r['input']) for r in records),
                         capture_output=True, text=True, timeout=90)
    if run.returncode: raise RuntimeError(run.stderr)
    actual = [json.loads(line) for line in run.stdout.splitlines()]
    if len(actual) != len(records): raise RuntimeError('Missing ACK results')
    failures = [dict(input=r['input'], expected=r['output'], actual=g)
                for r, g in zip(records, actual) if r['output'] != g]
    if a.failures: Path(a.failures).write_text(json.dumps(failures, indent=2))
    print(json.dumps(dict(cases=len(records), failures=len(failures), details=failures[:1]), indent=2))
    return bool(failures)
if __name__ == '__main__': raise SystemExit(main())
