#!/usr/bin/env python3
"""Execute original movement and real world collection in the same call."""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EAX
from original_world_reference import OriginalWorldQuery
from original_movement_request_reference import OriginalMovementRequest

class OriginalWorldMovement(OriginalWorldQuery):
    def __init__(self,binary):
        super().__init__(binary)
        self.return_hooks=set()
        self.cpu.hook_add(UC_HOOK_CODE,self._events,begin=0x6eb0b0,end=0x6eb0b0)
        self.cpu.hook_add(UC_HOOK_CODE,self._skip,begin=0x6e9b20,end=0x6e9b20)
        self.cpu.hook_add(UC_HOOK_CODE,self._observe_world,begin=0x7a5f20,end=0x7a5f20)
    _events=OriginalMovementRequest._events
    _skip=OriginalMovementRequest._skip
    def prepare_surface_world(self,case,body):
        self.setup_world(case['world'])
        for data in [0xadba34,0xadba54]:
            capacity=2048
            out=self.alloc(bytes(capacity*52));ids=self.alloc(bytes(capacity*8))
            self.cpu.mem_write(data,struct.pack('<8I',capacity,0,out,0,capacity,0,ids,0))
    def _observe_world(self,cpu,address,size,user):
        ret,body,query,data,flags,status=struct.unpack('<6I',cpu.mem_read(cpu.reg_read(UC_X86_REG_ESP),24))
        self.requests.append(dict(body=list(struct.unpack('<6f',cpu.mem_read(body,24))),box=list(struct.unpack('<6f',cpu.mem_read(query,24))),flags=flags))
        if ret not in self.return_hooks:
            self.cpu.hook_add(UC_HOOK_CODE,self._world_return,begin=ret,end=ret)
            self.return_hooks.add(ret)
    def _world_return(self,cpu,address,size,user):
        if not (cpu.reg_read(UC_X86_REG_EAX)&255):self.geometry_status=1
    def request(self,case):
        self.requests=[];self.events=[];self.geometry_status=0
        result=self.surface_move(dict(case,triangles=[],liquids=[]),routine=0x762e00)
        return dict(**result,requests=self.requests,events=self.events,geometry_status=self.geometry_status)
