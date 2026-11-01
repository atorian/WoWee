#!/usr/bin/env python3
"""Execute CollideRequestMove with supplied world and observed event boundary."""
import struct
from original_movement_query_reference import OriginalMovementQuery
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EIP,UC_X86_REG_EAX

class OriginalMovementRequest(OriginalMovementQuery):
    def __init__(self,binary):
        super().__init__(binary)
        self.cpu.hook_add(UC_HOOK_CODE,self._events,begin=0x6eb0b0,end=0x6eb0b0)
        self.cpu.hook_add(UC_HOOK_CODE,self._skip,begin=0x6e9b20,end=0x6e9b20)

    def _events(self,cpu,address,size,user_data):
        stack=cpu.reg_read(UC_X86_REG_ESP)
        ret,time,left,flags,flags2,jumping,transport=struct.unpack('<7I',cpu.mem_read(stack,28))
        if transport:raise RuntimeError('Attached transport outside request fixture')
        self.events.append(dict(kind=0,time=time,remaining=left,oldFlags=flags,oldFlags2=flags2,wasJumping=bool(jumping)))
        # Unit event delivery is outside this fixture. All movement routines,
        # including phase selection, time accounting and query refresh, run.
        cpu.reg_write(UC_X86_REG_ESP,stack+28);cpu.reg_write(UC_X86_REG_EIP,ret)

    def _skip(self,cpu,address,size,user_data):
        stack=cpu.reg_read(UC_X86_REG_ESP)
        amount=struct.unpack('<I',cpu.mem_read(stack+4,4))[0]
        self.events.append(dict(kind=1,time=amount,remaining=0,oldFlags=0,oldFlags2=0,wasJumping=False))

    def _world(self,cpu,address,size,user_data):
        stack=cpu.reg_read(UC_X86_REG_ESP)
        ret,body,query,data,flags,status=struct.unpack('<6I',cpu.mem_read(stack,24))
        self.requests.append(dict(body=list(struct.unpack('<6f',cpu.mem_read(body,24))),
                                  box=list(struct.unpack('<6f',cpu.mem_read(query,24))),flags=flags))
        fail=len(self.requests)==self.fail_query
        triangles=[] if fail else self.liquids if data==0xadba54 else self.solids
        facets=self.alloc(b''.join(struct.pack('<13f',*(t['plane']+sum(t['vertices'],[]))) for t in triangles) or bytes(16))
        ids=self.alloc(b''.join(struct.pack('<Q',t.get('owner',0)) for t in triangles) or bytes(8))
        cpu.mem_write(data,struct.pack('<8I',len(triangles),len(triangles),facets,0,len(triangles),len(triangles),ids,0))
        if fail:self.geometry_status=1
        cpu.reg_write(UC_X86_REG_EAX,int(not fail))
        cpu.reg_write(UC_X86_REG_ESP,stack+4);cpu.reg_write(UC_X86_REG_EIP,ret)

    def request(self,case):
        self.requests=[];self.events=[];self.geometry_status=0
        self.solids=case['triangles'];self.liquids=case.get('liquids',[])
        self.fail_query=case.get('fail_query',0)
        state=self.surface_move(dict(case,triangles=[],liquids=[]),routine=0x762e00)
        return dict(**state,requests=self.requests,events=self.events,geometry_status=self.geometry_status)
