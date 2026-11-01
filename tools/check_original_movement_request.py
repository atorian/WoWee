#!/usr/bin/env python3
"""Compare unattached request dispatch, queries and event-boundary observations."""
import argparse,json,random,subprocess,math
from pathlib import Path
from check_original_collision import f32,vector,quad
from check_original_swim import probe_input as swim_input
from check_original_surface import matches


def probe_input(c):
    return swim_input(c).rstrip()+' '+' '.join(map(str,[c.get('hover_height',0),int(c.get('local',False)),int(c.get('ghost',False)),c.get('fail_query',0)]))+'\n'


def cases():
    floor=quad([-30,-30,0],[30,-30,0],[30,30,0],[-30,30,0])
    wall=quad([0,-20,-10],[0,-20,30],[0,20,30],[0,20,-10])
    water=quad([-30,-30,2],[30,-30,2],[30,30,2],[-30,30,2])
    rng=random.Random(12340)
    for i in range(1600):
        mode=i%8;elapsed=rng.choice([0,1,16,100,500,1000])
        flags=[1,0x1001,0x200001,0x2000001,0x40000001,0x20001001,0x1005,0x801][mode]
        z=rng.uniform(.01,4) if mode else 0
        pos=[-rng.uniform(.1,4),rng.uniform(-3,3),z]
        speed=[7,7,4.72,7,7,7,7,0][mode]
        angle=rng.uniform(-1,1);direction=[math.cos(angle),math.sin(angle),rng.uniform(-1,1) if mode in (2,3) else 0]
        yield dict(name=f'request_{i}',position=vector(pos),radius=f32(1/3),height=f32(2.0277777),step_height=1,
                   flags=flags,flags2=0,elapsed=elapsed,distance=0,direction=[1,0],
                   swim_direction=vector([v*elapsed*.001*speed for v in direction]),current_speed=f32(speed),
                   move_direction=vector(direction),move_direction_2d=vector(direction[:2]),
                   fall_start=f32(z),fall_velocity=f32(-7.9555473 if i%3==0 else 0),fall_time=0,
                   hover_height=1,anchor_elapsed=elapsed,speeds=[2.5,7,4.5,4.72,2.5,7,4.5],
                   triangles=[] if i%5==0 else floor+(wall if i%2 else []),liquids=water,
                   client_controlled=True,local=bool(i%2),ghost=bool(i%11==0),fail_query=1 if i%13==0 else 0)

    for i in range(1600):
        angle=rng.uniform(0,82);rise=math.tan(math.radians(angle))
        ramp=quad([-10,-5,-10*rise],[10,-5,10*rise],[10,5,10*rise],[-10,5,-10*rise])
        flags=rng.choice([1,0x1001,0x200001,0x2000001,0x40000001,0x104001,0x100005])
        azimuth=rng.uniform(-math.pi,math.pi);direction=[math.cos(azimuth),math.sin(azimuth),0]
        elapsed=rng.choice([1,16,100,500])
        yield dict(name=f'request_slope_{i}',position=vector([-.4,0,2]),radius=f32(rng.uniform(.15,.8)),
                   height=f32(rng.uniform(1,4)),step_height=f32(rng.uniform(.3,2)),client_controlled=bool(i%3),
                   flags=flags,flags2=0,elapsed=elapsed,distance=0,direction=[1,0],swim_direction=vector([v*elapsed*.007 for v in direction]),
                   current_speed=7,move_direction=vector(direction),move_direction_2d=vector(direction[:2]),fall_start=2,
                   fall_velocity=f32(rng.choice([0,-7.9555473,5])),hover_height=f32(rng.uniform(.1,4)),anchor_elapsed=elapsed,
                   speeds=[2.5,7,4.5,4.72,2.5,7,4.5],triangles=ramp,liquids=water,local=True,ghost=False,
                   fail_query=rng.choice([0,0,0,2,3]))
    for x in [-17066.666,-16995.0,16995.0,17066.666]:
        for move in [-2,0,2]:
            yield dict(name=f'world_edge_{x}_{move}',position=vector([x,0,0]),radius=f32(1/3),height=2,
                       elapsed=100,flags=1,direction=[1,0],distance=0,swim_direction=[move,0,0],triangles=[],liquids=[])


def trajectories(original):
    floor=quad([-30,-30,0],[30,-30,0],[30,30,0],[-30,30,0])
    for jump in [False,True]:
        for schedule in [[16],[7,23,11,31],[100]]:
            c=dict(position=[0,0,0 if jump else 5],radius=f32(1/3),height=f32(2.0277777),flags=0x1001,
                   direction=[1,0],distance=0,triangles=floor,liquids=[],current_speed=7,
                   move_direction=[1,0,0],move_direction_2d=[1,0],fall_start=0 if jump else 5,
                   fall_velocity=f32(-7.9555473) if jump else 0,speeds=[2.5,7,4.5,4.72,2.5,7,4.5],time=1000)
            for frame in range(100):
                elapsed=schedule[frame%len(schedule)]
                c.update(name=f'request_trajectory_{jump}_{schedule[0]}_{frame}',elapsed=elapsed,swim_direction=[f32(elapsed*.007),0,0])
                output=original.request(c)
                yield dict(input=dict(c),output=output)
                c.update({k:v for k,v in output.items() if k not in ('name','elapsed','transport','fall_reset','requests','events','geometry_status')})
                c['time']+=output['elapsed']
                if not output['flags']&0x1000:break
            else:raise RuntimeError('Original request did not land')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures')
    args=p.parse_args()
    if args.reference:records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_movement_request_reference import OriginalMovementRequest
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementRequest(args.binary)
        records=[dict(input=c,output=original.request(c)) for c in cases()]
        records.extend(trajectories(original))
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CollideRequestMove 0x762e00',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe,'--request'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode:raise RuntimeError(result.stderr)
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Request probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures:Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
