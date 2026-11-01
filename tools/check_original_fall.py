#!/usr/bin/env python3
"""Compare the resident-geometry falling phase with original 12340 code."""
import argparse,json,math,random,subprocess
from pathlib import Path
from check_original_collision import quad,triangle,vector,f32
from check_original_surface import probe_input as surface_input,matches


def probe_input(c):
    extra=[c.get('time',1000),*c.get('ground_normal',[0,0,0]),*c.get('speeds',[0]*7)]
    return surface_input(c).rstrip()+' '+' '.join(map(str,extra))+'\n'


def cases():
    floor=quad([-30,-30,0],[30,-30,0],[30,30,0],[-30,30,0])
    wall=quad([0,-20,-10],[0,-20,30],[0,20,30],[0,20,-10])
    def case(name,position,elapsed,geometry,**kw):
        angle=kw.pop('angle',0)
        direction=vector([math.cos(angle),math.sin(angle)])
        return dict(name=name,position=vector(position),direction=direction,
                    distance=f32(kw.pop('distance',elapsed*.007)),radius=f32(1/3),height=f32(2.0277777),
                    elapsed=elapsed,flags=kw.pop('flags',0x1001),fall_start=kw.pop('fall_start',position[2]),
                    current_speed=7,move_direction=direction+[0],move_direction_2d=direction,
                    ground_normal=[.1,.2,.3],facing=f32(angle),triangles=geometry,**kw)
    for velocity in [0,-7.9555473,5,60,100]:
        for elapsed in [0,1,7,16,100,500,1000]:
            for time in [0,100,499,1000,5000]:
                yield case(f'empty_{velocity}_{elapsed}_{time}',[0,0,10],elapsed,[],fall_velocity=f32(velocity),fall_time=time)
    for height in [0,.001,.1,.5,1,3,10]:
        for elapsed in [1,16,100,500,1000]:
            for velocity in [0,-7.9555473]:
                yield case(f'land_{height}_{elapsed}_{velocity}',[-.5,0,height],elapsed,floor,fall_velocity=f32(velocity))
    for ceiling in [2.03,2.1,2.5,3,4]:
        geometry=floor+quad([-20,-20,ceiling],[-20,20,ceiling],[20,20,ceiling],[20,-20,ceiling])
        for elapsed in [16,100,500,1000]:
            yield case(f'ceiling_{ceiling}_{elapsed}',[-.5,0,0],elapsed,geometry,fall_velocity=f32(-7.9555473))
    for angle in [0,30,49.99,50,50.01,60,79.99,80,80.01]:
        rise=math.tan(math.radians(angle));ramp=quad([-10,-10,-10*rise],[10,-10,10*rise],[10,10,10*rise],[-10,10,-10*rise])
        for elapsed in [16,100,500]:
            yield case(f'ramp_{angle}_{elapsed}',[0,0,1],elapsed,ramp)
    for flags in [0x1001,0x3001,0x20001001,0x101001]:
        for elapsed in [16,500,1000]:yield case(f'flags_{flags}_{elapsed}',[-.5,0,1],elapsed,floor,flags=flags)
    rng=random.Random(12340)
    for i in range(400):
        elapsed=rng.randrange(1,501)
        geometry=floor+wall if i%2 else floor
        yield case(f'fall_{i}',[-rng.uniform(.1,3),rng.uniform(-2,2),rng.uniform(0,5)],elapsed,geometry,
                   angle=rng.uniform(-1,1),fall_velocity=f32(rng.choice([0,-7.9555473,10])))

    for pending in [0x4000,0x8000,0x10000,0x20000,0x40000,0x80000,0x100000,0xfc000]:
        for angle in [0,.7,2.1]:
            for flags2 in [0,1]:
                yield case(f'pending_{pending}_{angle}_{flags2}',[0,0,1],500,floor,
                           angle=angle,flags=0x1005|pending,flags2=flags2,
                           speeds=[2.5,7,4.5,4.72,2.5,7,4.5])
    for i in range(2000):
        angle=rng.uniform(-math.pi,math.pi);co,si=math.cos(angle),math.sin(angle)
        origin=[rng.uniform(-10000,10000),rng.uniform(-10000,10000),rng.uniform(-300,300)] if i%2 else [0,0,0]
        def transform(v):return [origin[0]+co*v[0]-si*v[1],origin[1]+si*v[0]+co*v[1],origin[2]+v[2]]
        slope=rng.uniform(0,82);rise=math.tan(math.radians(slope))
        geometry=quad([-10,-3,-10*rise],[10,-3,10*rise],[10,3,10*rise],[-10,3,-10*rise])
        geometry=[triangle(*[transform(v) for v in t['vertices']]) for t in geometry]
        for t in geometry:t['owner']=0xf110000000001234 if i%3==0 else 0
        yield case(f'transformed_{i}',transform([-.4,rng.uniform(-3.4,3.4),2]),rng.randrange(1,501),geometry,
                   angle=angle,fall_velocity=f32(rng.choice([0,-7.9555473,5])),client_controlled=bool(i%3))


def trajectory_cases(original):
    floor=quad([-30,-30,0],[30,-30,0],[30,30,0],[-30,30,0])
    for schedule in [[16],[7,23,11,31],[100]]:
        for jump in [False,True]:
            c=dict(position=[0,0,0 if jump else 10],fall_start=0 if jump else 10,
                   radius=f32(1/3),height=f32(2.0277777),direction=[1,0],move_direction=[1,0,0],
                   move_direction_2d=[1,0],flags=0x1001,fall_velocity=f32(-7.9555473) if jump else 0,
                   current_speed=7,speeds=[2.5,7,4.5,4.72,2.5,7,4.5],triangles=floor,time=1000)
            for frame in range(400):
                dt=schedule[frame%len(schedule)]
                c.update(name=f'trajectory_{schedule[0]}_{jump}_{frame}',elapsed=dt,distance=f32(dt*.007))
                output=original.fall_move(c)
                yield dict(input=dict(c),output=output)
                if not output['flags']&0x1000:break
                c.update({k:v for k,v in output.items() if k not in ('name','elapsed','transport','fall_reset')})
                c['time']+=output['elapsed']
            else:raise RuntimeError('Original falling trajectory did not land')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    source=p.add_mutually_exclusive_group(required=True);source.add_argument('--binary');source.add_argument('--reference')
    output=p.add_mutually_exclusive_group(required=True);output.add_argument('--probe');output.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures');p.add_argument('--edges',action='store_true')
    args=p.parse_args()
    if args.reference:records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_collision_reference import OriginalCollision,REFERENCE_SHA256
        original=OriginalCollision(args.binary)
        selected=cases
        if args.edges:
            from itertools import chain
            from fall_edge_cases import cases as edge_cases,boundary_cases
            selected=lambda:chain(edge_cases(),boundary_cases())
        records=[dict(input=c,output=original.fall_move(c)) for c in selected()]
        if not args.edges:records.extend(trajectory_cases(original))
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='Fall 0x7618b0',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe,'--fall'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode:raise RuntimeError(f'Fall probe failed: {result.returncode}: {result.stderr}')
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Fall probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures:Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=[dict(name=f['input']['name'],expected=f['expected'],actual=f['actual']) for f in failures[:3]]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
