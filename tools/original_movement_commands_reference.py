#!/usr/bin/env python3
"""Execute original height enqueue and event delivery, observing the unit boundary."""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EIP,UC_X86_REG_EAX
from original_collision_reference import OriginalCollision

class OriginalMovementCommands(OriginalCollision):
    def __init__(self,binary):
        super().__init__(binary)
        self.cpu.hook_add(UC_HOOK_CODE,self._local_event,begin=0x7413f0,end=0x7413f0)
        self.cpu.hook_add(UC_HOOK_CODE,self._globals,begin=0x74b330,end=0x74b330)
    def write(self,at,fmt,*values):self.cpu.mem_write(at,struct.pack('<'+fmt,*values))
    def read(self,at,fmt):return struct.unpack('<'+fmt,self.cpu.mem_read(at,struct.calcsize(fmt)))
    def _globals(self,cpu,address,size,user):
        sp=cpu.reg_read(UC_X86_REG_ESP);ret=self.read(sp,'I')[0]
        cpu.reg_write(UC_X86_REG_EAX,self.globals)
        cpu.reg_write(UC_X86_REG_ESP,sp+4);cpu.reg_write(UC_X86_REG_EIP,ret)
    def _local_event(self,cpu,address,size,user):
        # ProcessLocalMoveEvent is the external unit/network boundary. Record
        # its arguments AND the mover visible at notification time; no claim
        # about actual packet bytes or acknowledgement admission is made here.
        sp=cpu.reg_read(UC_X86_REG_ESP);args=self.read(sp,'9I')
        value=struct.unpack('<f',struct.pack('<I',args[4]))[0]
        self.notifications.append([*args[1:4],value,args[5],args[6]|args[7]<<32,args[8],
            self.read(self.body+0xcc,'f')[0],self.read(self.body+0x134,'I')[0],self.read(self.body+0x48,'H')[0]])
        cpu.reg_write(UC_X86_REG_EAX,int(bool(args[3])))
        cpu.reg_write(UC_X86_REG_ESP,sp+36);cpu.reg_write(UC_X86_REG_EIP,args[0])
    def prepare_surface_world(self,case,body):
        self.prepare_queue(case,body)
    def prepare_queue(self,case,body):
        self.body=body;self.globals=self.alloc(bytes(0x200))
        self.passenger=self.alloc(bytes(0x40))
        self.unit=self.read(body+0x144,'I')[0]
        self.write(self.unit+0xf60,'I',self.passenger)
        for at,offset in [(body+0x138,0),(self.globals+0x118,0x30)]:
            self.write(at,'3I',offset,at+4,(at+4)|1)
        # Supply storage only. Original AllocEvent, Enqueue and list operations
        # run unmodified, including recycling delivered events to this list.
        free=0xada370;nodes=[self.alloc(bytes(0x60)) for _ in range(len(case['operations'])+1)]
        self.write(free,'3I',0,nodes[-1],nodes[0])
        for i,node in enumerate(nodes):
            self.write(node,'2I',nodes[i-1] if i else free+4,
                nodes[i+1] if i+1<len(nodes) else (free+4)|1)
    def run(self,case):
        self.notifications=[]
        self.surface_move(dict(case,position=[0,0,0],radius=.33333334,triangles=[],direction=[0,0],
            distance=0,elapsed=0),routine=0x6eac40)
        updates=[]
        for op in case['operations']:
            if op[0]==0:
                _,time,height,counter,ack=op;bits=struct.unpack('<I',struct.pack('<f',height))[0]
                self.call(0x6ee760 if ack else 0x6ee7b0,[time,counter,bits] if ack else [time,bits],self.body)
            else:
                _,time,defer=op;self.notifications=[]
                self.write(self.passenger+0x10,'I',0x200 if defer else 0)
                result=self.call(0x6ef860,[time],self.body)
                node=self.read(self.body+0x140,'I')[0];times=[]
                while node and not node&1:
                    times.append(self.read(node+8,'I')[0]);node=self.read(node+4,'I')[0]
                updates.append(dict(notifications=self.notifications,result=result,
                    height=self.read(self.body+0xcc,'f')[0],delay=self.read(self.body+0x134,'I')[0],
                    flags2=self.read(self.body+0x48,'H')[0],pending=len(times),next=times[0] if times else None))
        return dict(name=case['name'],updates=updates)
