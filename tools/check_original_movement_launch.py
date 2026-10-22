#!/usr/bin/env python3
"""Compare knockback state, timed dispatch and full frames with original 12340."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_surface import matches
from check_original_movement_state import cases as state_cases
from check_original_movement_rate_queue import cases as rate_cases
from check_original_movement_input_queue import probe_input as queue_input
from check_original_world_movement import apply_cases,probe_input as frame_input

def cases(queue,execute):
    rng=random.Random(0x6e9ff0)
    if queue:
        for i,c in enumerate(rate_cases(execute)):
            when=(c['last_time']+rng.choice([0,1,8,16,100]))&0xffffffff if execute else c['time']
            launch=[when,*vector([rng.uniform(-1,1),rng.uniform(-1,1)]),f32(rng.uniform(0,20)),f32(rng.uniform(-20,5)),rng.getrandbits(32)]
            yield dict(c,name=f'launch_queue_{execute}_{i}',inputs=[*c['inputs'],launch])
        return
    for i,c in enumerate(state_cases()):
        if i>=2048:break
        yield dict(c,name=f'launch_{i}',launch_direction=vector([rng.uniform(-2,2),rng.uniform(-2,2)]),
            launch_speed=f32(rng.uniform(-10,30)),launch_vertical=f32(rng.uniform(-30,30)))
    base=next(apply_cases())
    for flags in [0,1,2,0x1000,0x1001,0x200000,0x2000000,0x2200000,0x800,0x100000,0x200,0x400,0x40000000,0x4000000,0x3000800,0x3201800]:
        for direction in [[0,0],[1,0],[-1,0],[0,1],[0,-1],[.5,.5],[2,-2],[1e-8,1e-8]]:
            for vertical in [0,-7,7,-20]:
                yield dict(base,name=f'launch_boundary_{flags}_{direction}_{vertical}'.replace(' ', ''),flags=flags,
                    flags2=0x20,position=[1,2,3],pitch=f32(.3),facing=f32(.7),
                    launch_direction=direction,launch_speed=7,launch_vertical=vertical)

def trajectories(original):
    for mode in [0,0x800,0x200000,0x2000000]:
        c=next(apply_cases())
        c['world']=dict(c['world'],wmo=1,area=3,dungeon=False,doodad_flags=0x80,owner=0,
            entity_flags=2,provider=True,available=True,impassable=False,holes=0,flight=False)
        c.update(execute_input=True,active=True,local=True,position=[-5,-35,6],anchor_position=[-5,-35,6],
            anchor_elapsed=0,flags=mode,flags2=0,fall_start=6,fall_velocity=0,
            facing=0,anchor_facing=0,pitch=0,anchor_pitch=0,pitch_cos=1,pitch_sin=0,
            move_direction=[0,0,0],move_direction_2d=[0,0],current_speed=0,move_start_delay=0,
            last_time=0xfffffff0,heartbeat_time=0x100,idle_time=0xffffffef,defer=False)
        for frame in range(40):
            elapsed=[16,23,100,251][frame%4];last=c['last_time'];time=(last+elapsed//2)&0xffffffff
            inputs=[[time,[0,2,3,5][frame%4],1]]
            if frame%10==0:inputs.append([time,1,0,7,-7,frame])
            c.update(name=f'launch_trajectory_{mode}_{frame}',elapsed=elapsed,time=(last+elapsed)&0xffffffff,inputs=inputs)
            output=original.run(c);yield dict(input=dict(c),output=output)
            exclude={'name','elapsed','transport','fall_reset','requests','events','geometry_status',
                'notifications','heartbeats','finished','retire','angular_stops','rate_states'}
            c.update({k:v for k,v in output.items() if k not in exclude});c['last_time']=c['time']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    p.add_argument('--queue',action='store_true');p.add_argument('--execute',action='store_true')
    p.add_argument('--probe');p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    queue=a.queue or a.execute
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_launch_reference import OriginalMovementLaunch
        from original_movement_rate_queue_reference import OriginalMovementRateQueue
        from original_collision_reference import REFERENCE_SHA256
        original=(OriginalMovementRateQueue if queue else OriginalMovementLaunch)(a.binary)
        records=[]
        for c in cases(queue,a.execute):
            try:records.append(dict(input=c,output=original.run(c)))
            except Exception as error:raise RuntimeError(c['name']) from error
        if a.execute:records.extend(trajectories(original))
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,fpu_control=0x27f,
                routines='Launch 0x6e9ff0; optionally OnLaunchLocal 0x6ecc80, original dispatch and ExecuteMovement; unattached, no spline, observed unit boundary'),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if not a.probe:
        print(f'Generated {len(records)} original knockback cases');return 0
    def line(c):
        if queue:return queue_input(c)
        return frame_input(c).rstrip()+' '+' '.join(map(str,[*c['launch_direction'],c['launch_speed'],c['launch_vertical']]))+'\n'
    flag='--world-rate-execute' if a.execute else '--world-rate-queue' if queue else '--world-launch'
    run=subprocess.run([a.probe,flag],input=''.join(line(r['input']) for r in records),capture_output=True,text=True,timeout=90)
    if run.returncode:raise RuntimeError(run.stderr)
    actual=[json.loads(row) for row in run.stdout.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing launch results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2));return bool(failures)
if __name__=='__main__':raise SystemExit(main())
