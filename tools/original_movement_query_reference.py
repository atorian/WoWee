#!/usr/bin/env python3
"""Observe original GetMoveFacets requests at its world-service boundary."""
import struct
from original_collision_reference import OriginalCollision
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX

class OriginalMovementQuery(OriginalCollision):
    def __init__(self,binary):
        super().__init__(binary)
        self.cpu.hook_add(UC_HOOK_CODE,self._world,begin=0x783910,end=0x783910)

    def _world(self,cpu,address,size,user_data):
        stack=cpu.reg_read(UC_X86_REG_ESP)
        ret,body,query,data,flags,status=struct.unpack('<6I',cpu.mem_read(stack,24))
        self.requests.append(dict(body=list(struct.unpack('<6f',cpu.mem_read(body,24))),
                                  box=list(struct.unpack('<6f',cpu.mem_read(query,24))),flags=flags))
        # World loading is a supplied service, not part of this query-envelope
        # reference. Return a resident empty world without replacing mover math.
        cpu.mem_write(data+4,bytes(4));cpu.mem_write(data+20,bytes(4))
        cpu.reg_write(UC_X86_REG_EAX,1)
        cpu.reg_write(UC_X86_REG_ESP,stack+4);cpu.reg_write(UC_X86_REG_EIP,ret)

    def query(self,case):
        self.requests=[]
        self.surface_move(case,routine=0x75f0a0 if case.get('cached_box') else 0x75ff90)
        result=dict(name=case['name'],requests=self.requests)
        if case.get('cached_box'):
            result['cache']=list(struct.unpack('<6f',self.cpu.mem_read(0xca1660,24)))
        return result
