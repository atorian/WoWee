#!/usr/bin/env python3
"""Compare original mover-to-wire bytes and decoding of original payloads."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector

def probe_input(c,original):
    values=[c['flags'],c['flags2'],c['time'],*c['position'],c['facing'],c['pitch'],c['fall_time'],
        c['fall_velocity'],*c['move_direction_2d'],c['current_speed'],c['step_start'],original]
    return c['name']+' '+' '.join(map(str,values))+'\n'
def cases():
    rng=random.Random(0x7164b0)
    for i in range(1024):
        modes=[0,1,0x1000,0x1001,0x200000,0x2000000,0x4000000,0x4001000]
        flags=modes[i%8]|[0,0x200,0x8000000,0x80000000][i//8%4]
        yield dict(name=f'wire_status_{i}',flags=flags,flags2=[0,0x20,0x400,0x1c20][i//32%4],
            time=rng.getrandbits(32),position=vector([rng.uniform(-10000,10000) for _ in range(3)]),
            facing=f32(rng.uniform(-7,7)),pitch=f32(rng.uniform(-1.5,1.5)),fall_time=rng.getrandbits(32),
            fall_velocity=f32(rng.uniform(-10,60)),move_direction_2d=vector([rng.uniform(-1,1),rng.uniform(-1,1)]),
            current_speed=f32(rng.uniform(0,50)),step_start=f32(rng.uniform(-10,10)),
            radius=f32(1/3),height=f32(2.0277777),direction=[0,0],distance=0,triangles=[])
def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_status_reference import OriginalMovementStatus
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementStatus(a.binary)
        records=[dict(input=c,output=original.run(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routines='WriteMovementStatusToPacket 0x7164b0, GetMoveStatus and real CMovementStatus/CDataStore serialization; unattached mover, no spline',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input'],r['output']['written']) for r in records),capture_output=True,text=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing wire status results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if r['output']!=g]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
