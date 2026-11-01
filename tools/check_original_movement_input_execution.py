#!/usr/bin/env python3
"""Compare timed player inputs through the original scheduler and collision world."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_surface import matches
from check_original_collision import f32
from check_original_world_movement import apply_cases
from check_original_movement_input_queue import probe_input

def cases():
    rng=random.Random(0x6ec840)
    for i,c in enumerate(apply_cases()):
        elapsed=rng.choice([1,7,16,100,249,250,251,500]);last=[1000,0xfffffff0][i%2]
        flags=[0,0x800,0x1000,0x200000,0x2000000,0x40000000][i%6]
        inputs=[]
        for j in range(1+i%8):
            inputs.append([(last+rng.choice([0,1,elapsed//2,elapsed,elapsed+1]))&0xffffffff,
                rng.randrange(16),rng.randrange(2)])
            if (i+j)%3==0:
                inputs.append([inputs[-1][0],f32(rng.uniform(.5,5)),rng.getrandbits(32),rng.randrange(2)])
        yield dict(c,name=f'input_execution_{i}',execute_input=True,flags=flags,
            elapsed=elapsed,time=(last+elapsed)&0xffffffff,last_time=last,
            heartbeat_time=(last+500)&0xffffffff,idle_time=last,defer=bool(i%23==0),inputs=inputs)
def trajectories(original,flags2=0):
    for mode in [0,0x1000,0x200000,0x2000000,0x800]:
        for turning in [False,True]:
            c=next(apply_cases())
            c['world']=dict(c['world'],wmo=1,area=3,dungeon=False,doodad_flags=0x80,owner=0,
                entity_flags=2,provider=True,available=True,impassable=False,holes=0,flight=False)
            c.update(execute_input=True,active=True,local=True,position=[-5,-35,6],anchor_position=[-5,-35,6],
                anchor_elapsed=0,flags=mode,flags2=flags2,fall_start=6,fall_velocity=0,
                facing=0,anchor_facing=0,pitch=0,anchor_pitch=0,pitch_cos=1,pitch_sin=0,
                move_direction=[0,0,0],move_direction_2d=[0,0],current_speed=0,move_start_delay=0,
                last_time=0xfffffff0,heartbeat_time=0x100,idle_time=0xffffffef,defer=False)
            for frame in range(30):
                elapsed=[7,23,16,100,251][frame%5];last=c['last_time']
                commands=[0,15,1,2,3,5,4,5,12,14,13,14,9,11,10,11]
                inputs=[[(last+elapsed//2)&0xffffffff,commands[frame%len(commands)],1]]
                if frame%3!=0:inputs.insert(0,[(last+elapsed//2)&0xffffffff,[.75,1.5,4][frame%3],frame,1])
                if turning:inputs.append([(last+elapsed//3)&0xffffffff,[6,7,8][frame%3],1])
                c.update(name=f'input_trajectory_{mode}_{turning}_{frame}',elapsed=elapsed,
                    time=(last+elapsed)&0xffffffff,inputs=inputs)
                output=original.run(c)
                yield dict(input=dict(c),output=output)
                exclude={'name','elapsed','transport','fall_reset','requests','events','geometry_status',
                    'notifications','heartbeats','finished','retire','angular_stops','rate_states','mode_observations'}
                c.update({k:v for k,v in output.items() if k not in exclude})
                c['last_time']=c['time']
def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_input_queue_reference import OriginalMovementInputQueue
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementInputQueue(a.binary);records=[]
        for c in cases():
            try:records.append(dict(input=c,output=original.run(c)))
            except Exception as error:raise RuntimeError(c['name']) from error
        records.extend(trajectories(original))
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routines='ExecuteMovement, queued basic inputs, ApplyMovement and real world collector; observed unit/input delivery; successful heartbeat preparation service',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe,'--world-input-execute'],input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing input execution results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
