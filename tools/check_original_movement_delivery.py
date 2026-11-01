#!/usr/bin/env python3
"""Compare original collision frames through real local delivery and wire bytes."""
import argparse
import json
import subprocess
from pathlib import Path
from check_original_surface import matches
from itertools import chain
from check_original_movement_mode_queue import cases
from check_original_movement_input_execution import cases as input_cases, trajectories
from check_original_movement_input_queue import probe_input as queue_input

def probe_input(c):
    initial=c.get('reporting',{})
    values=[initial.get('unit_flags',0x400 if c.get('local',False) else 0),
            initial.get('reported_facing',c.get('facing',0)),initial.get('reported_pitch',c.get('pitch',0)),
            initial.get('turn_time',0),initial.get('pitch_time',0),initial.get('pending_turn',0),initial.get('pending_pitch',0)]
    return queue_input(c).rstrip()+' '+' '.join(map(str,values))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True)
    src.add_argument('--binary');src.add_argument('--reference')
    p.add_argument('--probe');p.add_argument('--opcodes',default='Data/expansions/wotlk/opcodes.json')
    p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_delivery_reference import OriginalMovementDelivery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementDelivery(a.binary);records=[]
        for c in chain(cases(True),(dict(c,flags2=c.get('flags2',0)|24) for c in input_cases())):
            try:records.append(dict(input=c,output=original.run(c)))
            except Exception as error:raise RuntimeError(c['name']) from error
        records.extend(trajectories(original,24))
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,fpu_control=0x27f,
                routines='ExecuteMovement, ApplyMovement, real world collection, UpdatePlayerMovement, ProcessLocalMoveEvent, SendMovementUpdate and real packet bytes; inline unattached mover, no spline; unit/input effects and lifecycle are observed'),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if not a.probe:
        print(f'Generated {len(records)} original delivered frames');return 0
    run=subprocess.run([a.probe,'--world-delivery',a.opcodes],input=''.join(probe_input(r['input']) for r in records),capture_output=True,text=True,timeout=90)
    if run.returncode:raise RuntimeError(run.stderr)
    actual=[json.loads(line) for line in run.stdout.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing delivered frames')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:1]),indent=2));return bool(failures)
if __name__=='__main__':raise SystemExit(main())
