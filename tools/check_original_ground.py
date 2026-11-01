#!/usr/bin/env python3
"""Compare original 12340 ground-normal query on resident facets."""
import argparse
import json
import math
from pathlib import Path
import random
import subprocess
from check_original_collision import quad, triangle, f32, vector
from check_original_fall import probe_input as fall_input
from check_original_surface import matches


def probe_input(c):
    return fall_input(c)


def cases():
    def case(name, position, geometry, **kw):
        return dict(name=name,position=vector(position),direction=[1,0],distance=0,
                    radius=f32(1/3),height=f32(2.0277777),elapsed=0,flags=1,triangles=geometry,**kw)
    yield case('empty',[0,0,0],[])
    for angle in [0,30,50,80,88.999,89,89.001,90]:
        rise=math.tan(math.radians(angle)) if angle<90 else 1000000
        plane=quad([-1,-1,-rise],[1,-1,rise],[1,1,rise],[-1,1,-rise])
        for z in [-2.029,-2.0278,-2,0,.001,.00139,.002,1]:
            yield case(f'slope_{angle}_{z}',[0,0,z],plane)
    rng=random.Random(12340)
    for i in range(1000):
        origin=[rng.uniform(-10000,10000),rng.uniform(-10000,10000),rng.uniform(-300,300)] if i%2 else [0,0,0]
        geometry=[]
        for j in range(rng.randrange(1,12)):
            azimuth=rng.uniform(-math.pi,math.pi);co,si=math.cos(azimuth),math.sin(azimuth)
            rise=math.tan(rng.uniform(0,1.56));base=rng.uniform(-.2,2.2)
            def transform(v):return [origin[0]+co*v[0]-si*v[1],origin[1]+si*v[0]+co*v[1],origin[2]+base+v[2]]
            plane=quad([-1,-1,-rise],[1,-1,rise],[1,1,rise],[-1,1,-rise])
            geometry.extend(triangle(*[transform(v) for v in t['vertices']]) for t in plane)
        if i%3==0:geometry.reverse()
        yield case(f'junction_{i}',origin,geometry)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    source=p.add_mutually_exclusive_group(required=True);source.add_argument('--binary');source.add_argument('--reference')
    output=p.add_mutually_exclusive_group(required=True);output.add_argument('--probe');output.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures')
    args=p.parse_args()
    if args.reference: records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_collision_reference import OriginalCollision, REFERENCE_SHA256
        original=OriginalCollision(args.binary)
        records=[dict(input=c, output=original.ground_move(c)) for c in cases()]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='GroundNormal 0x75ee60',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual: text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe,'--ground'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode: raise RuntimeError(f'Ground probe failed: {result.returncode}: {result.stderr}')
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records): raise RuntimeError('Ground probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures: Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=[dict(name=f['input']['name'],expected=f['expected'],actual=f['actual']) for f in failures[:3]]),indent=2))
    return bool(failures)


if __name__=='__main__': raise SystemExit(main())
