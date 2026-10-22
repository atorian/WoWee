#!/usr/bin/env python3
"""Execute original map dispatch; observe its geometry service boundaries."""
import struct
from original_terrain_reference import OriginalTerrainQuery
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EIP,UC_X86_REG_EAX

class OriginalMapQuery(OriginalTerrainQuery):
    def __init__(self,binary):
        super().__init__(binary)
        for address in [0x7a55e0,0x7a5a60,0x7a4590]:
            self.cpu.hook_add(UC_HOOK_CODE,self._collect,begin=address,end=address)

    def _collect(self,cpu,address,size,user_data):
        stack=cpu.reg_read(UC_X86_REG_ESP)
        args=struct.unpack('<8I',cpu.mem_read(stack,32))
        if address==0x7a55e0:
            call=[0,0,0,0,0,0,0,args[4]];data=args[3]
        elif address==0x7a5a60:
            y0,x0,y1,x1=struct.unpack('<4i',cpu.mem_read(args[3],16))
            x,y=struct.unpack('<2i',cpu.mem_read(stack+4,8))
            call=[1,x,y,x0,y0,x1,y1,args[7]];data=args[6]
        else:
            x,y=struct.unpack('<2i',cpu.mem_read(stack+4,8))
            call=[2,x,y,0,0,0,0,0];data=args[4]
        # Supplied geometry appends one face, even on failure. This checks the
        # original dispatch's clear/partial-output behaviour, not face math.
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        cpu.mem_write(data+4,struct.pack('<I',count+1))
        success=len(self.calls)!=self.failed
        self.calls.append(call)
        cpu.reg_write(UC_X86_REG_EAX,int(success))
        cpu.reg_write(UC_X86_REG_ESP,stack+4)
        cpu.reg_write(UC_X86_REG_EIP,args[0])

    def query(self,case):
        self.next=0x2000000;self.calls=[];self.failed=case['failed']
        box=self.floats(case['box']);body=self.floats([0]*6)
        data=self.alloc(struct.pack('<8I',4096,4,self.alloc(bytes(4096*52)),0,0,0,0,0))
        self.cpu.mem_write(0xcf08f4,struct.pack('<I',case['dungeon']))
        result=self.call(0x7a5f20,[body,box,data,case['flags'],0])
        count=struct.unpack('<I',self.cpu.mem_read(data+4,4))[0]
        return dict(name=case['name'],ready=bool(result&255),facets=count,calls=self.calls)
