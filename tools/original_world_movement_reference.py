#!/usr/bin/env python3
"""Execute original movement and real world collection in the same call."""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EAX,UC_X86_REG_EIP
from original_world_reference import OriginalWorldQuery
from original_movement_request_reference import OriginalMovementRequest

class OriginalWorldMovement(OriginalWorldQuery):
    def __init__(self,binary):
        super().__init__(binary)
        self.cpu.hook_add(UC_HOOK_CODE,self._globals,begin=0x74b330,end=0x74b330)
        self.cpu.hook_add(UC_HOOK_CODE,self._unit_skip,begin=0x717d90,end=0x717d90)
        self.cpu.hook_add(UC_HOOK_CODE,self._object_lookup,begin=0x4d4db0,end=0x4d4db0)
        self.return_hooks=set()
        self.cpu.hook_add(UC_HOOK_CODE,self._request_return,begin=0x6eae05,end=0x6eae05)
        self.cpu.hook_add(UC_HOOK_CODE,self._events,begin=0x6eb0b0,end=0x6eb0b0)
        self.cpu.hook_add(UC_HOOK_CODE,self._skip,begin=0x6e9b20,end=0x6e9b20)
        self.cpu.hook_add(UC_HOOK_CODE,self._observe_world,begin=0x7a5f20,end=0x7a5f20)
    _events=OriginalMovementRequest._events
    _skip=OriginalMovementRequest._skip
    def _object_lookup(self,cpu,address,size,user):
        # The fixture has no game transport objects. Keep the real SetTransport
        # failure path; a scene facet owner is not an attached passenger frame.
        sp=cpu.reg_read(UC_X86_REG_ESP);ret=struct.unpack('<I',cpu.mem_read(sp,4))[0]
        cpu.reg_write(UC_X86_REG_EAX,0);cpu.reg_write(UC_X86_REG_ESP,sp+4);cpu.reg_write(UC_X86_REG_EIP,ret)
    def _globals(self,cpu,address,size,user):
        # Thread-owned movement globals are an external service. The original
        # SkipTime still executes and updates its heartbeat clock in this block.
        sp=cpu.reg_read(UC_X86_REG_ESP);ret=struct.unpack('<I',cpu.mem_read(sp,4))[0]
        cpu.reg_write(UC_X86_REG_EAX,self.globals);cpu.reg_write(UC_X86_REG_ESP,sp+4);cpu.reg_write(UC_X86_REG_EIP,ret)
    def _unit_skip(self,cpu,address,size,user):
        # Unit/network notification is outside this movement oracle, just as
        # CallMoveEventHandlers is. SkipTime itself is recorded by _skip.
        sp=cpu.reg_read(UC_X86_REG_ESP);ret=struct.unpack('<I',cpu.mem_read(sp,4))[0]
        cpu.reg_write(UC_X86_REG_ESP,sp+8);cpu.reg_write(UC_X86_REG_EIP,ret)
    def _request_return(self,cpu,address,size,user):
        self.consumed+=cpu.reg_read(UC_X86_REG_EAX)
    def prepare_surface_world(self,case,body):
        self.setup_world(case['world'])
        self.globals=self.alloc(bytes(0x200))
        self.cpu.mem_write(0xca1238,struct.pack('<Q',1 if case.get('active',False) else 0))
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
        self.requests=[];self.events=[];self.geometry_status=0;self.consumed=0
        result=self.surface_move(dict(case,triangles=[],liquids=[]),routine=0x6eac40 if case.get('apply',False) else 0x762e00)
        if case.get('apply',False):result['elapsed']=self.consumed
        return dict(**result,requests=self.requests,events=self.events,geometry_status=self.geometry_status)
