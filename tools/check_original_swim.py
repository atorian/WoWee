#!/usr/bin/env python3
"""Compare original 12340 swimming/flight on resident solid and liquid facets."""
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
    return fall_input(c).rstrip() + ' ' + ' '.join(map(str, extra)) + '\n'


def cases():
    floor = quad([-30,-30,-3],[30,-30,-3],[30,30,-3],[-30,30,-3])
    water = quad([-30,-30,0],[-30,30,0],[30,30,0],[30,-30,0])
    wall = quad([0,-20,-10],[0,-20,30],[0,20,30],[0,20,-10])
    ceiling = quad([-30,-30,3],[-30,30,3],[30,30,3],[30,-30,3])
    def case(name, position, direction, distance, solids, liquids, **kw):
        return dict(name=name, position=vector(position), direction=[1,0], swim_direction=vector(direction),
                    distance=f32(distance), radius=f32(1/3), height=f32(2.0277777), elapsed=kw.pop('elapsed',16),
                    flags=kw.pop('flags',0x200001), current_speed=f32(4.72), speeds=[2.5,7,4.5,4.72,2.5,7,4.5],
                    triangles=solids, liquids=liquids, **kw)
    for flags in [0x200001,0x600001,0x2000001,0x2400001,0x200401]:
        for flags2 in [0,2,4,0x20]:
            for z in [-4,-2,-1,0,1]:
                for direction in [[0,0,1],[0,0,-1],[.8,0,.6]]:
                    yield case(f'water_{flags}_{flags2}_{z}_{direction[2]}',[-1,0,z],direction,.75,floor,water,flags=flags,flags2=flags2)
    for elapsed in [0,1,16,1000]:
        for distance in [0,1e-7,1e-6,.001,.01,.1,1,10]:
            yield case(f'empty_{elapsed}_{distance}',[10000,-9000,100],[.6,0,.8],distance,[],[],elapsed=elapsed)
    rng = random.Random(12340)
    for i in range(1000):
        azimuth=rng.uniform(-math.pi,math.pi);pitch=rng.uniform(-math.pi/2,math.pi/2)
        direction=[math.cos(azimuth)*math.cos(pitch),math.sin(azimuth)*math.cos(pitch),math.sin(pitch)]
        solids=floor+(wall if i%2 else [])+(ceiling if i%3==0 else [])
        yield case(f'swim_{i}',[-rng.uniform(.1,3),rng.uniform(-1,1),rng.uniform(-2.9,1)],direction,rng.uniform(.001,5),solids,water,
                   flags=rng.choice([0x200001,0x600001,0x2000001,0x2400001]),elapsed=rng.choice([1,16,100,500]))
    for i in range(1000):
        azimuth=rng.uniform(-math.pi,math.pi);co,si=math.cos(azimuth),math.sin(azimuth)
        origin=[rng.uniform(-10000,10000),rng.uniform(-10000,10000),rng.uniform(-300,300)]
        slope=rng.uniform(0,85);rise=math.tan(math.radians(slope))
        def transform(v):return [origin[0]+co*v[0]-si*v[1],origin[1]+si*v[0]+co*v[1],origin[2]+v[2]]
        ramp=quad([-10,-5,-10*rise],[10,-5,10*rise],[10,5,10*rise],[-10,5,-10*rise])
        ramp=[triangle(*[transform(v) for v in t['vertices']]) for t in ramp]
        pitch=rng.uniform(-1,1)
        yield case(f'flight_{i}',transform([-.5,0,1]),[co*math.cos(pitch),si*math.cos(pitch),math.sin(pitch)],rng.uniform(.001,5),ramp,[],flags=0x2000001)


def trajectory_cases(original):
    floor = quad([-30,-30,-3],[30,-30,-3],[30,30,-3],[-30,30,-3])
    water = quad([-30,-30,0],[-30,30,0],[30,30,0],[30,-30,0])
    wall = quad([0,-20,-10],[0,-20,30],[0,20,30],[0,20,-10])
    for ascending in [False, True]:
        for dt in [7,16,100]:
            c=dict(position=[-1,0,-2], direction=[1,0], swim_direction=vector([.8,0,.6]),
                   radius=f32(1/3), height=f32(2.0277777), flags=0x600001 if ascending else 0x200001,
                   current_speed=f32(4.72), speeds=[2.5,7,4.5,4.72,2.5,7,4.5],triangles=floor+wall,liquids=water,
                   elapsed=dt, distance=f32(dt*.00472), time=1000)
            for frame in range(120):
                c['name']=f'trajectory_{ascending}_{dt}_{frame}'
                out=original.swim_move(c)
                yield dict(input=dict(c), output=out)
                if out['flags']&0x1000:break
                c.update({k:v for k,v in out.items() if k not in ('name','elapsed','transport','fall_reset')})
                c['time']+=dt


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
        records=[dict(input=c, output=original.swim_move(c)) for c in cases()]
        records.extend(trajectory_cases(original))
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='Swim 0x760b40',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual: text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe,'--swim'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode: raise RuntimeError(f'Swim probe failed: {result.returncode}: {result.stderr}')
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records): raise RuntimeError('Swim probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures: Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=[dict(name=f['input']['name'],expected=f['expected'],actual=f['actual']) for f in failures[:3]]),indent=2))
    return bool(failures)


if __name__=='__main__': raise SystemExit(main())
