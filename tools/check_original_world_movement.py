#!/usr/bin/env python3
"""Compare movement, real world collection and movement-event boundaries together."""
import argparse,json,random,math,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_movement_request import probe_input as request_input
from check_original_world import cases as world_cases,probe_input as world_input
from check_original_surface import matches

def probe_input(c):
    world=world_input(c['world']).split()[1:13]
    return request_input(c).rstrip()+' '+' '.join(world)+'\n'

def cases():
    rng=random.Random(852691)
    for i,w in enumerate(world_cases()):
        elapsed=rng.choice([1,16,100,500]);angle=rng.uniform(-math.pi,math.pi)
        mode=i%7;flags=[1,0x1001,0x200001,0x2000001,0x40000001,0x20001001,0x801][mode]
        position=vector([rng.uniform(-9,-1),rng.uniform(-39,-31),rng.uniform(.01,7)])
        direction=vector([math.cos(angle),math.sin(angle),rng.uniform(-1,1) if mode in (2,3) else 0])
        yield dict(name=f'world_movement_{i}',world=w,position=position,radius=f32(1/3),height=f32(2.0277777),step_height=1,
            direction=direction[:2],swim_direction=vector([v*elapsed*.007 for v in direction]),distance=0,elapsed=elapsed,flags=flags,
            current_speed=7,move_direction=direction,move_direction_2d=direction[:2],fall_start=position[2],fall_time=0,
            fall_velocity=f32(-7.9555473 if i%3==0 else 0),hover_height=1,anchor_elapsed=elapsed,
            speeds=[2.5,7,4.5,4.72,2.5,7,4.5],triangles=[],liquids=[],local=bool(i%2),ghost=bool(i%11==0))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_world_movement_reference import OriginalWorldMovement
        from original_collision_reference import REFERENCE_SHA256
        o=OriginalWorldMovement(a.binary)
        records=[dict(input=c,output=o.request(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CollideRequestMove 0x762e00 with real GetFacets 0x7a5f20; supplied entity collision callback and observed movement-event delivery',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        result=subprocess.run([a.probe,'--world-request'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode:raise RuntimeError(result.stderr)
        text=result.stdout
    actual=[json.loads(l) for l in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing world movement results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
