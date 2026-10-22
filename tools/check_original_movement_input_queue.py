#!/usr/bin/env python3
"""Compare original queued input admission, transitions and notification order."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_surface import matches
from check_original_world_movement import probe_input as frame_input
from check_original_movement_input import cases as input_cases

def probe_input(c):
    extra=[c['last_time'],c['heartbeat_time'],c['idle_time'],int(c['defer']),len(c['inputs'])]
    for op in c['inputs']:
        if isinstance(op,dict):extra.extend([4,op['time'],op['mode'],int(op['enabled']),op['counter']])
        else:extra.append(len(op)-3);extra.extend(op)
    return frame_input(c).rstrip()+' '+' '.join(map(str,extra))+'\n'
def cases():
    rng=random.Random(0x6ef860)
    for i,c in enumerate(input_cases()):
        now=[1000,0xfffffff0,0x80000010][i%3]
        inputs=[[now,c['input'],i%2]]
        if i<2048:
            for j in range(i%7):
                inputs.append([(now+rng.choice([-1,0,0,1,10,0x7fffffff]))&0xffffffff,rng.randrange(16),rng.randrange(2)])
        yield dict(c,name=f'input_queue_{i}',time=now,last_time=(now-16)&0xffffffff,
            heartbeat_time=(now-1)&0xffffffff,idle_time=now,active=bool(i%3),defer=bool(i%19==0),inputs=inputs)
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
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routines='AddPlayerMoveEvent, Enqueue and UpdatePlayerMovement; observed unit delivery reports no send, observed angular control stop',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe,'--world-input-queue'],input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing queued input results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
