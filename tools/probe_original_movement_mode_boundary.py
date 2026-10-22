#!/usr/bin/env python3
"""Reproduce the unresolved input/unit boundary; this is not a passing parity test."""
import argparse,json
from pathlib import Path
from unicorn.x86_const import UC_X86_REG_EIP
from original_movement_mode_queue_reference import OriginalMovementModeQueue

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',required=True)
    p.add_argument('--case',default='tests/fixtures/original_mode_input_boundary_12340.json');a=p.parse_args()
    fixture=json.loads(Path(a.case).read_text());original=OriginalMovementModeQueue(a.binary)
    try:
        result=original.run(fixture['input'])
        print(json.dumps(dict(completed=True,result=result),indent=2));return 0
    except RuntimeError as error:
        if str(error)!='Original surface exceeded its instruction budget':raise
        print(json.dumps(dict(completed=False,instruction_limit=fixture['instruction_limit'],
            eip=hex(original.cpu.reg_read(UC_X86_REG_EIP)),flags=hex(original.read(original.body+0x44,'I')[0]),
            observed_boundary='Unit/input effects are stubbed; complete original-client behavior remains unresolved'),indent=2))
        return 1
if __name__=='__main__':raise SystemExit(main())
