#!/usr/bin/env python3
"""Compare queued speed/rate changes, alone and through complete movement frames."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32
from check_original_surface import matches
from check_original_movement_state import cases as state_cases
from check_original_movement_input_execution import cases as execution_cases
from check_original_movement_input_queue import probe_input
from check_original_world_movement import apply_cases

def cases(execute):
    rng=random.Random(0x6edb30)
    if execute:
        for i,c in enumerate(execution_cases()):
            inputs=[]
            for op in c['inputs']:
                inputs.extend([op,[op[0],(i+len(inputs))%9,f32(rng.uniform(0,15)),rng.getrandbits(32),rng.randrange(2)]])
            yield dict(c,name=f'rate_execution_{i}',inputs=inputs)
        return
    for i,c in enumerate(state_cases()):
        if i>=2304:break
        now=[1000,0xfffffff0,0x80000010][i%3]
        inputs=[[now,i%9,f32(rng.uniform(0,20)),rng.getrandbits(32),i%2]]
        for j in range(i%6):
            time=(now+rng.choice([-1,0,0,1,100,0x7fffffff]))&0xffffffff
            inputs.extend([[time,rng.randrange(16),rng.randrange(2)],
                [time,rng.randrange(9),f32(rng.uniform(0,20)),rng.getrandbits(32),rng.randrange(2)]])
            if j%2:inputs.append([time,f32(rng.uniform(.5,4)),rng.getrandbits(32),rng.randrange(2)])
        yield dict(c,name=f'rate_queue_{i}',time=now,last_time=(now-16)&0xffffffff,
            heartbeat_time=(now-1)&0xffffffff,idle_time=now,active=bool(i%3),defer=bool(i%19==0),inputs=inputs)

def trajectories(original):
    for mode in [0,0x1000,0x200000,0x2000000,0x800,0x40000000]:
        c=next(apply_cases())
        c['world']=dict(c['world'],wmo=1,area=3,dungeon=False,doodad_flags=0x80,owner=0,
            entity_flags=2,provider=True,available=True,impassable=False,holes=0,flight=False)
        c.update(execute_input=True,active=True,local=True,position=[-5,-35,6],anchor_position=[-5,-35,6],
            anchor_elapsed=0,flags=mode,flags2=0,fall_start=6,fall_velocity=0,
            facing=0,anchor_facing=0,pitch=0,anchor_pitch=0,pitch_cos=1,pitch_sin=0,
            move_direction=[0,0,0],move_direction_2d=[0,0],current_speed=0,move_start_delay=0,
            last_time=0xfffffff0,heartbeat_time=0x100,idle_time=0xffffffef,defer=False)
        for frame in range(30):
            elapsed=[7,23,16,100,251][frame%5];last=c['last_time']
            time=(last+elapsed//2)&0xffffffff
            inputs=[[time,[0,15,1,2,3,5,12,14,13,14][frame%10],1],
                [time,frame%9,[0,1,4.5,7,14][frame%5],frame,1],
                [time,[6,7,8][frame%3],1]]
            c.update(name=f'rate_trajectory_{mode}_{frame}',elapsed=elapsed,time=(last+elapsed)&0xffffffff,inputs=inputs)
            output=original.run(c)
            yield dict(input=dict(c),output=output)
            exclude={'name','elapsed','transport','fall_reset','requests','events','geometry_status',
                'notifications','heartbeats','finished','retire','angular_stops','rate_states'}
            c.update({k:v for k,v in output.items() if k not in exclude});c['last_time']=c['time']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    p.add_argument('--execute',action='store_true');p.add_argument('--probe');p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_rate_queue_reference import OriginalMovementRateQueue
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementRateQueue(a.binary);records=[]
        for c in cases(a.execute):
            try:records.append(dict(input=c,output=original.run(c)))
            except Exception as error:raise RuntimeError(c['name']) from error
        if a.execute:records.extend(trajectories(original))
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,fpu_control=0x27f,
                routines='Original speed/rate enqueue, UpdatePlayerMovement and optionally ExecuteMovement/ApplyMovement; unit notification observed, successful heartbeat preparation supplied'),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if not a.probe:
        print(f'Generated {len(records)} original rate queue cases');return 0
    run=subprocess.run([a.probe,'--world-rate-execute' if a.execute else '--world-rate-queue'],
        input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
    if run.returncode:raise RuntimeError(run.stderr)
    actual=[json.loads(line) for line in run.stdout.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing rate queue results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2));return bool(failures)
if __name__=='__main__':raise SystemExit(main())
