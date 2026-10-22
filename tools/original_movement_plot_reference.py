#!/usr/bin/env python3
"""Execute original PlotUnitMovement, including its original trigonometry."""
import struct,ctypes
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EBP,UC_X86_REG_EAX,UC_X86_REG_EDX,UC_X86_REG_EIP
from original_collision_reference import OriginalCollision
class OriginalMovementPlot(OriginalCollision):
    def __init__(self,binary,cpu_reference=None):
        super().__init__(binary)
        self.cpu_reference=None
        if cpu_reference:
            self.cpu_reference=ctypes.CDLL(str(Path(cpu_reference).resolve()))
            self.cpu_reference.original_fsincos.argtypes=[ctypes.c_uint32]
            self.cpu_reference.original_fsincos.restype=ctypes.c_uint64
            for address in [0x9878ca,0x9878e5,0x9879ad,0x987a60]:
                self.cpu.hook_add(UC_HOOK_CODE,self.sincos,begin=address,end=address)
    def sincos(self,cpu,address,size,user):
        # Replace only FLD/FSINCOS/two stores with that host instruction's
        # binary32 stores; all surrounding original arithmetic still executes.
        offset={0x9878ca:0x10,0x9878e5:0x1c,0x9879ad:8,0x987a60:0x10}[address]
        bits=struct.unpack('<I',cpu.mem_read(cpu.reg_read(UC_X86_REG_EBP)-offset,4))[0]
        result=self.cpu_reference.original_fsincos(bits)
        cpu.mem_write(cpu.reg_read(UC_X86_REG_EAX),struct.pack('<I',result>>32))
        cpu.mem_write(cpu.reg_read(UC_X86_REG_EDX),struct.pack('<I',result&0xffffffff))
        cpu.reg_write(UC_X86_REG_EIP,address+9)
    def plot(self,c):
        self.next=0x2000000;cpu=self.cpu;body=self.alloc(bytes(0x200))
        def write(offset,fmt,*values):cpu.mem_write(body+offset,struct.pack('<'+fmt,*values))
        write(0x44,'IH',c['flags'],c['flags2']);write(0x58,'2f',c['anchor_facing'],c['anchor_pitch'])
        write(0x64,'3f',*c['direction']);write(0x70,'2f',*c['direction2d']);write(0x78,'2f',c['pitch_cos'],c['pitch_sin'])
        write(0x8c,'f',c['speed']);write(0xac,'2f',c['turn_rate'],c['pitch_rate'])
        move=self.floats(c['move']);facing=self.floats([c['facing']]);pitch=self.floats([c['pitch']])
        result=self.call(0x987b50,[c['elapsed'],move,facing if c['write_facing'] else 0,pitch if c['write_pitch'] else 0],body)
        return dict(name=c['name'],active=result,move=list(struct.unpack('<3f',cpu.mem_read(move,12))),
            facing=struct.unpack('<f',cpu.mem_read(facing,4))[0],pitch=struct.unpack('<f',cpu.mem_read(pitch,4))[0])
