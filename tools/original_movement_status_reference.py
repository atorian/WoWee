#!/usr/bin/env python3
"""Run original GetMoveStatus and WriteMovementStatusToPacket into a real buffer."""
import struct
from original_movement_input_reference import OriginalMovementInput

class OriginalMovementStatus(OriginalMovementInput):
    def run(self,case):
        self.surface_move(dict(case,elapsed=0),routine=0x6eac40)
        # Preallocate external CDataStore storage. All status and serialization
        # instructions run unchanged; no writer, field or arithmetic is hooked.
        buffer=self.alloc(bytes(1024))
        store=self.alloc(struct.pack('<6I',0x9e0e24,buffer,0,1024,0,0xffffffff))
        self.call(0x7164b0,[0xee,case['time'],store],self.body)
        length=struct.unpack('<I',self.cpu.mem_read(store+16,4))[0]
        if length>1024:raise RuntimeError('Movement status exceeded fixture storage')
        data=bytes(self.cpu.mem_read(buffer,length)).hex()
        return dict(name=case['name'],written=data,read_rewritten=data,read=True,remaining=0)
