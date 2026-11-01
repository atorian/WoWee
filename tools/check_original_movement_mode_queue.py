#!/usr/bin/env python3
"""Compare original mode command order, ACK boundaries and movement frames."""
import argparse,json,subprocess
from pathlib import Path
from check_original_surface import matches
from check_original_movement_launch import cases as launch_cases
from check_original_world_movement import apply_cases
from check_original_collision import f32
from check_original_movement_input_queue import probe_input

def cases(execute):
    if execute:
        # Full-frame mode matrix uses server commands. Arbitrary local-input
        # injection has an unresolved unit/input boundary recorded separately
        # in original_mode_input_boundary_12340.json; it is not a passing case.
        for i,c in enumerate(apply_cases()):
            last=[1000,0xfffffff0][i%2];elapsed=[7,16,100,250,500][i%5]
            when=(last+elapsed//2)&0xffffffff
            inputs=[dict(time=when,mode=i%7,enabled=bool(i//7%2),counter=i),
                [when,i%9,f32(2+i%13),i,1],
                [when,f32(.75+i%4),i,1]]
            if i%3==0:inputs.append(dict(time=(when+1)&0xffffffff,mode=(i+3)%7,enabled=bool(i%2),counter=i+1))
            if i%4==0:inputs.append([when,1,0,7,-7,i])
            yield dict(c,name=f'mode_execution_{i}',execute_input=True,
                flags=[1,0x1001,0x200001,0x2000001,0x800,0x40000001][i%6],flags2=c.get('flags2',0)|(4 if i%4==0 else 0),
                last_time=last,time=(last+elapsed)&0xffffffff,elapsed=elapsed,
                heartbeat_time=(last+500)&0xffffffff,idle_time=last,defer=bool(i%23==0),inputs=inputs)
        return
    for i,c in enumerate(launch_cases(True,False)):
        when=c['inputs'][0][0]
        mode=dict(time=when,mode=i%7,enabled=bool(i//7%2),counter=0x10203040+i)
        inputs=[mode,*c['inputs']]
        if i%3==0:inputs.append(dict(time=when,mode=(i+3)%7,enabled=not mode['enabled'],counter=i))
        yield dict(c,name=f'mode_queue_{execute}_{i}',inputs=inputs)

def trajectories(original):
    for mode in [1,0x800,0x200001,0x2000001,0x40000001,0x1001]:
        c=next(apply_cases())
        c['world']=dict(c['world'],wmo=1,area=3,dungeon=False,doodad_flags=0x80,owner=0,
            entity_flags=2,provider=True,available=True,impassable=False,holes=0,flight=False)
        c.update(execute_input=True,active=True,local=True,position=[-5,-35,6],anchor_position=[-5,-35,6],
            anchor_elapsed=0,flags=mode,flags2=0,fall_start=6,fall_velocity=0,
            facing=0,anchor_facing=0,pitch=0,anchor_pitch=0,pitch_cos=1,pitch_sin=0,
            move_direction=[1,0,0],move_direction_2d=[1,0],current_speed=7,move_start_delay=0,
            last_time=0xfffffff0,heartbeat_time=0x100,idle_time=0xffffffef,defer=False)
        for frame in range(30):
            elapsed=[7,23,16,100,251][frame%5];last=c['last_time'];when=(last+elapsed//2)&0xffffffff
            inputs=[dict(time=when,mode=frame%7,enabled=bool(frame//7%2),counter=frame),
                [when,frame%9,[1,4.5,7,14][frame%4],frame,1]]
            if frame%10==0:inputs.append([when,1,0,7,-7,frame])
            c.update(name=f'mode_trajectory_{mode}_{frame}',elapsed=elapsed,time=(last+elapsed)&0xffffffff,inputs=inputs)
            output=original.run(c);yield dict(input=dict(c),output=output)
            exclude={'name','elapsed','transport','fall_reset','requests','events','geometry_status',
                'notifications','heartbeats','finished','retire','angular_stops','rate_states','mode_observations'}
            c.update({k:v for k,v in output.items() if k not in exclude});c['last_time']=c['time']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    p.add_argument('--execute',action='store_true');p.add_argument('--probe');p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_mode_queue_reference import OriginalMovementModeQueue
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementModeQueue(a.binary);records=[]
        for c in cases(a.execute):
            try:records.append(dict(input=c,output=original.run(c)))
            except Exception as error:raise RuntimeError(c['name']) from error
        if a.execute:records.extend(trajectories(original))
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,fpu_control=0x27f,
                routines='Original server mode enqueue/dispatch with gravity and hover helpers; unit/input effects and queued CallMoveEventHandlers observed, no spline/attached transport'),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if not a.probe:
        print(f'Generated {len(records)} original mode command cases');return 0
    flag='--world-mode-execute' if a.execute else '--world-mode-queue'
    run=subprocess.run([a.probe,flag],input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
    if run.returncode:raise RuntimeError(run.stderr)
    actual=[json.loads(line) for line in run.stdout.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing mode command results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2));return bool(failures)
if __name__=='__main__':raise SystemExit(main())
