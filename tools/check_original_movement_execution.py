#!/usr/bin/env python3
"""Compare original frame scheduling with queued heights and real world collisions."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32
from check_original_surface import matches
from check_original_world_movement import apply_cases,probe_input as frame_input

def probe_input(c):
    extra=[c['last_time'],c['heartbeat_time'],c['idle_time'],int(c['defer']),len(c['operations'])]
    for op in c['operations']:extra.extend(op[1:])
    return frame_input(c).rstrip()+' '+' '.join(map(str,extra))+'\n'
def cases():
    rng=random.Random(0x6f09f0)
    for i,c in enumerate(apply_cases()):
        elapsed=rng.choice([0,1,7,16,100,249,250,251,500,1000])
        last=[1000,0xfffffff0,0x7ffffff0][i%3]
        c=dict(c,name=f'execute_{i}',elapsed=elapsed,last_time=last,time=(last+elapsed)&0xffffffff,
            heartbeat_time=(last+rng.choice([1,7,50,249,250,500,-1]))&0xffffffff,
            idle_time=(last-1)&0xffffffff,defer=bool(i%17==0),operations=[])
        # A frame starts with the prior height already applied. Queue arrivals
        # include old, equal, intermediate and future times, in arrival order.
        for j in range(i%6):
            dt=rng.choice([-1,0,1,7,7,16,249,250,251,500])
            c['operations'].append([0,(last+dt)&0xffffffff,f32(rng.uniform(.5,5)),rng.getrandbits(32),rng.randrange(2)])
        yield c
def trajectories(original):
    for turning in [False,True]:
        for mode in [0x1001,0x200001,0x2000001]:
            c=next(apply_cases())
            c['world']=dict(c['world'],wmo=1,area=3,dungeon=False,doodad_flags=0x80,owner=0,
                entity_flags=2,provider=True,available=True,impassable=False,holes=0,flight=False)
            c.update(active=True,local=True,position=[-5,-35,6],anchor_position=[-5,-35,6],anchor_elapsed=0,
                flags=mode|(0x10 if turning else 0),flags2=0,fall_start=6,fall_velocity=0,
                facing=0,anchor_facing=0,pitch=0,anchor_pitch=0,pitch_cos=1,pitch_sin=0,
                move_direction=[1,0,0],move_direction_2d=[1,0],current_speed=7,move_start_delay=0,
                last_time=0xfffffff0,heartbeat_time=0x20,idle_time=0xffffffef,defer=False)
            for frame in range(30):
                elapsed=[7,23,16,100,251][frame%5];last=c['last_time']
                c.update(name=f'execute_trajectory_{turning}_{mode}_{frame}',elapsed=elapsed,
                    time=(last+elapsed)&0xffffffff,
                    operations=[[0,(last+elapsed//2)&0xffffffff,[.75,1.5,4][frame%3],frame,1]])
                output=original.run(c)
                yield dict(input=dict(c),output=output)
                exclude={'name','elapsed','transport','fall_reset','requests','events','geometry_status',
                    'notifications','heartbeats','finished','retire'}
                c.update({k:v for k,v in output.items() if k not in exclude})
                c['last_time']=c['time']
def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_execution_reference import OriginalMovementExecution
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementExecution(a.binary);records=[]
        for c in cases():
            try:records.append(dict(input=c,output=original.run(c)))
            except Exception as error:raise RuntimeError(c['name']) from error
        records.extend(trajectories(original))
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routines='ExecuteMovement 0x6f09f0, ApplyMovement, real world collectors, height command queue, SendMovementUpdate and UpdateHeartbeatTimerA; supplied successful packet preparation; observed unit and retirement boundaries',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe,'--world-execute'],input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing execution results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
