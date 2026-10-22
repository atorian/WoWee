#!/usr/bin/env python3
"""Compare original 12340 hover support and movement on resident facets."""
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
    extra = [*c['swim_direction'], len(c.get('liquids', []))]
    for t in c.get('liquids', []):
        extra += t['plane'] + [t.get('owner', 0)] + sum(t['vertices'], [])
    return fall_input(c).rstrip() + ' ' + ' '.join(map(str, extra)) + ' ' + str(c['hover_height']) + '\n'


def cases():
    floor=quad([-30,-30,0],[30,-30,0],[30,30,0],[-30,30,0])
    wall=quad([0,-20,-10],[0,-20,30],[0,20,30],[0,20,-10])
    def case(name, position, direction, distance, geometry, **kw):
        return dict(name=name,position=vector(position),direction=[1,0],swim_direction=vector(direction),
                    distance=f32(distance),radius=f32(1/3),height=f32(2.0277777),elapsed=kw.pop('elapsed',16),
                    flags=kw.pop('flags',0x40000001),hover_height=kw.pop('hover_height',1),current_speed=7,
                    speeds=[2.5,7,4.5,4.72,2.5,7,4.5],triangles=geometry,**kw)
    for hover in [0,.1,1,3]:
        for z in [0,.01,.1,.5,1,2,4,10]:
            for elapsed in [0,1,16,500]:
                for geometry,name in [(floor,'floor'),([],'empty')]:
                    yield case(f'{name}_{hover}_{z}_{elapsed}',[-1,0,z],[1,0,0],f32(elapsed*.007),geometry,hover_height=hover,elapsed=elapsed)
    rng=random.Random(12340)
    for i in range(2000):
        angle=rng.uniform(-math.pi,math.pi);co,si=math.cos(angle),math.sin(angle)
        origin=[rng.uniform(-10000,10000),rng.uniform(-10000,10000),rng.uniform(-300,300)] if i%2 else [0,0,0]
        def transform(v):return [origin[0]+co*v[0]-si*v[1],origin[1]+si*v[0]+co*v[1],origin[2]+v[2]]
        slope=rng.uniform(0,82);rise=math.tan(math.radians(slope))
        geometry=quad([-10,-3,-10*rise],[10,-3,10*rise],[10,3,10*rise],[-10,3,-10*rise])
        geometry=[triangle(*[transform(v) for v in t['vertices']]) for t in geometry]
        for t in geometry:t['owner']=0xf110000000001234 if i%3==0 else 0
        yield case(f'hover_{i}',transform([-.4,rng.uniform(-3.4,3.4),rng.uniform(.1,4)]),[co,si,0],rng.uniform(.001,4),geometry,
                   hover_height=f32(rng.uniform(.1,3)),client_controlled=bool(i%3),elapsed=rng.choice([1,16,100,500]))


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
        records=[dict(input=c, output=original.hover_move(c)) for c in cases()]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='HoverMove 0x762980',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual: text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe,'--hover'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode: raise RuntimeError(f'Hover probe failed: {result.returncode}: {result.stderr}')
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records): raise RuntimeError('Hover probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures: Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=[dict(name=f['input']['name'],expected=f['expected'],actual=f['actual']) for f in failures[:3]]),indent=2))
    return bool(failures)


if __name__=='__main__': raise SystemExit(main())
