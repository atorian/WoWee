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
    extra=[int(c['active']),c['move_start_delay'],c['turn_rate'],c['pitch_rate'],c['pitch_cos'],c['pitch_sin']] if c.get('apply',False) else []
    return request_input(c).rstrip()+' '+' '.join(world)+' '+ ' '.join(map(str,extra))+'\n'

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

def apply_cases():
    rng=random.Random(45634)
    for i,c in enumerate(cases()):
        c=dict(c,apply=True,active=bool(i%3),move_start_delay=rng.choice([0,499,500,999,1499,1500,1999]),
            anchor_position=c['position'],anchor_elapsed=rng.choice([0,16,100,500]),
            turn_rate=f32(rng.uniform(.2,6)),pitch_rate=f32(rng.uniform(.2,6)),pitch_cos=1,pitch_sin=0)
        c['name']='apply_'+c['name']
        c['flags']|=rng.choice([0,0x10,0x20])
        if c['flags']&(0x200000|0x2000000):c['flags']|=rng.choice([0,0x40,0x80,0x400000,0x800000])
        # ApplyMovement receives root state after TryRoot clears directional
        # input. The isolated request fixture also tests contradictory flags;
        # that synthetic tuple loops forever in the original frame dispatcher.
        if c['flags']&0x800:c['flags']=(c['flags']&0xff203700)|0x800
        c['flags2']=rng.choice([0,8,16,24]);c['facing']=c['anchor_facing']=f32(rng.uniform(-math.pi,math.pi))
        c['pitch']=c['anchor_pitch']=f32(rng.uniform(-.7,.7));c['pitch_cos']=f32(math.cos(c['pitch']));c['pitch_sin']=f32(math.sin(c['pitch']))
        yield c

    base=next(cases())
    base['world']=dict(base['world'],wmo=0,area=0,dungeon=True)
    for axis in [0,1]:
        for value in [-17066.666,-17064.667,-17064.666,-16995,16995,17064.664,17064.666,17066.666]:
            for elapsed in [0,1,16]:
                position=[-5,-35,4];position[axis]=f32(value)
                yield dict(base,name=f'apply_world_edge_{axis}_{value}_{elapsed}',apply=True,active=True,
                    position=position,anchor_position=position,anchor_elapsed=0,flags=1,flags2=0,
                    elapsed=elapsed,fall_start=4,move_start_delay=0,turn_rate=1,pitch_rate=1,pitch_cos=1,pitch_sin=0)
    for delay in [0,499,500,999,1499,1500,0x7fffffff,0x80000000,0xffffffff]:
        for anchor in [0,500,1000,0xffffffff]:
            for elapsed in [0,1,16,500]:
                yield dict(base,name=f'apply_counters_{delay}_{anchor}_{elapsed}',apply=True,active=False,
                    anchor_position=base['position'],anchor_elapsed=anchor,flags=1,flags2=0,elapsed=elapsed,
                    move_start_delay=delay,turn_rate=1,pitch_rate=1,pitch_cos=1,pitch_sin=0)


def apply_trajectories(original):
    for turning in [False,True]:
        for schedule in [[16],[7,23,11,31],[100]]:
            c=next(apply_cases())
            c['world']=dict(c['world'],wmo=1,area=3,dungeon=False,doodad_flags=0x80,owner=0,entity_flags=2,
                provider=True,available=True,impassable=False,holes=0,flight=False)
            c.update(active=True,local=True,position=[-5,-35,6],anchor_position=[-5,-35,6],anchor_elapsed=0,
                flags=0x1001|(0x10 if turning else 0),flags2=0,fall_start=6,fall_velocity=0,
                facing=0,anchor_facing=0,pitch=0,anchor_pitch=0,pitch_cos=1,pitch_sin=0,
                move_direction=[1,0,0],move_direction_2d=[1,0],current_speed=7,move_start_delay=0,time=1000)
            for frame in range(30):
                c.update(name=f'apply_trajectory_{turning}_{schedule[0]}_{frame}',elapsed=schedule[frame%len(schedule)])
                output=original.request(c)
                yield dict(input=dict(c),output=output)
                c.update({k:v for k,v in output.items() if k not in ('name','elapsed','transport','fall_reset','requests','events','geometry_status')})
                c['time']+=c['elapsed']


def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--apply',action='store_true');p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_world_movement_reference import OriginalWorldMovement
        from original_collision_reference import REFERENCE_SHA256
        o=OriginalWorldMovement(a.binary)
        records=[dict(input=c,output=o.request(c)) for c in (apply_cases() if a.apply else cases())]
        if a.apply:records.extend(apply_trajectories(o))
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine=('ApplyMovement 0x6eac40' if a.apply else 'CollideRequestMove 0x762e00')+' with real GetFacets 0x7a5f20; supplied entity collision callback, thread globals and empty transport registry; observed unit movement-event delivery',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        result=subprocess.run([a.probe,'--world-apply' if records[0]['input'].get('apply',False) else '--world-request'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode:raise RuntimeError(result.stderr)
        text=result.stdout
    actual=[json.loads(l) for l in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing world movement results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
