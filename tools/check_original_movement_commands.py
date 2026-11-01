#!/usr/bin/env python3
"""Compare queued height updates against original build 12340 execution."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32
from check_original_surface import matches

def probe_input(c):
    values=[c['flags'],c['flags2'],c['move_start_delay'],c['height'],len(c['operations'])]
    for op in c['operations']:values.extend(op)
    return c['name']+' '+' '.join(map(str,values))+'\n'
def cases():
    rng=random.Random(0x123406ee)
    for i in range(768):
        origin=[1000,0,0xffffffef,0x7ffffff0][i%4]
        operations=[]
        for block in range(4):
            for k in range(6):
                offset=rng.choice([-1,0,1,5,10,10,15,30,0x7fffffff,0x80000000])
                operations.append([0,(origin+offset)&0xffffffff,f32(rng.uniform(.01,9)),rng.getrandbits(32),rng.randrange(2)])
            for offset in [-1,0,1,10,30,31]:
                time=(origin+offset)&0xffffffff
                operations.append([1,time,rng.randrange(2)])
                operations.append([1,time,0])
            origin=(origin+32)&0xffffffff
        yield dict(name=f'height_queue_{i}',flags=[0,1,0x800,0x200,0x1000,0x40000000,0x200000,0x2000000][i%8],
            flags2=[0,0x1c00,0xffff][i%3],move_start_delay=rng.getrandbits(32),height=f32(2.0277777),operations=operations)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_commands_reference import OriginalMovementCommands
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementCommands(a.binary)
        records=[dict(input=c,output=original.run(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routines='OnSetCollisionHeight[Local], AllocEvent, Enqueue, UpdatePlayerMovement, SetInterpolation; observed ProcessLocalMoveEvent boundary',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing movement queue results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),updates=sum(len(r['output']['updates']) for r in records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
