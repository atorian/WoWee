#!/usr/bin/env python3
"""Compare original shared input state transitions, including pending airborne input."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_surface import matches
from check_original_world_movement import apply_cases,probe_input as frame_input

def probe_input(c):return frame_input(c).rstrip()+' '+str(c['input'])+'\n'
def cases():
    rng=random.Random(0x988a20);base=next(apply_cases())
    for i in range(2048):
        flags=rng.getrandbits(32)&~0x08000000
        yield dict(base,name=f'input_{i}',input=i%16,flags=flags,flags2=rng.getrandbits(16),
            position=vector([rng.uniform(-20,20) for _ in range(3)]),
            facing=f32(rng.uniform(-7,7)),pitch=f32(rng.uniform(-1.5,1.5)),
            current_speed=f32(rng.uniform(0,15)),anchor_elapsed=rng.getrandbits(32),
            speeds=vector([rng.uniform(0,20) for _ in range(7)]),
            move_start_delay=rng.getrandbits(32))
    modes=[0,1,2,3,4,8,12,15,0x1000,0x1001,0x1002,0x1004,0x1008,
        0x200000,0x200001,0x2000000,0x2000001,0x40000000,0x40000001,
        0x800,0x100000,0x400,0x4000,0x8000,0x10000,0x20000,0x40000,
        0x80000,0x4000000,0x4000001,0x4000004,0x1001000]
    for flags in modes:
        for flags2 in [0,1,2,4,0x20,0x1c00]:
            for command in range(16):
                yield dict(base,name=f'input_modes_{flags}_{flags2}_{command}',input=command,
                    flags=flags,flags2=flags2,position=[-5,-35,6],current_speed=7,
                    facing=f32(.7),pitch=f32(.3),anchor_elapsed=123,
                    speeds=[2.5,7,4.5,f32(4.72),2.5,7,4.5])
def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_input_reference import OriginalMovementInput
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementInput(a.binary)
        records=[dict(input=c,output=original.run(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routines='CMovementShared Start/StopMove, Strafe, Turn, Pitch, AscensionDescension and Jump; no spline',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe,'--world-input'],input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing movement input results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
