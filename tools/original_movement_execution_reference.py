#!/usr/bin/env python3
"""Original ExecuteMovement with real queue, ApplyMovement and world collection.

Unit notifications and packet encoding are external boundaries. The real
SendMovementUpdate and UpdateHeartbeatTimerA run with a supplied successful
packet-preparation service; this verifies scheduling, not packet bytes.
"""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EIP,UC_X86_REG_EAX
from original_world_movement_reference import OriginalWorldMovement
from original_movement_commands_reference import OriginalMovementCommands

class OriginalMovementExecution(OriginalWorldMovement):
    write=OriginalMovementCommands.write
    read=OriginalMovementCommands.read
    prepare_queue=OriginalMovementCommands.prepare_queue
    _local_event=OriginalMovementCommands._local_event
    observe_local_event=OriginalMovementCommands.observe_local_event
    def __init__(self,binary):
        super().__init__(binary)
        for address,callback in [(0x7413f0,self._local_event),(0x73ab20,self._finished),
            (0x6eaf50,self._retire),(0x71f0c0,self._send),(0x71ef80,self._prepare_packet),
            (0x6b0b50,self._network)]:
            self.cpu.hook_add(UC_HOOK_CODE,callback,begin=address,end=address)
    def _return(self,cpu,count):
        sp=cpu.reg_read(UC_X86_REG_ESP);ret=self.read(sp,'I')[0]
        cpu.reg_write(UC_X86_REG_ESP,sp+4+count);cpu.reg_write(UC_X86_REG_EIP,ret)
    def _finished(self,cpu,address,size,user):
        self.finished.append(self.read(cpu.reg_read(UC_X86_REG_ESP)+4,'I')[0]);self._return(cpu,12)
    def _retire(self,cpu,address,size,user):
        # Scheduler requests retirement; actual list lifetime, hover support
        # and spline finalisation belong to the world-owner boundary.
        self.retire=True;self._return(cpu,4)
    def _send(self,cpu,address,size,user):
        args=self.read(cpu.reg_read(UC_X86_REG_ESP),'8I')
        self.heartbeats.append([args[1],self.read(self.body+0xcc,'f')[0],
            *self.read(self.body+0x10,'3f'),self.read(self.body+0x44,'I')[0]])
        if args[2:]!=(0xee,0,0,0,0,255):raise RuntimeError('Unexpected movement update')
    def _prepare_packet(self,cpu,address,size,user):
        args=self.read(cpu.reg_read(UC_X86_REG_ESP),'6I')
        # No actual buffer was allocated: -1 prevents destructor ownership.
        self.write(args[3]+0xc,'I',0xffffffff)
        cpu.reg_write(UC_X86_REG_EAX,1);self._return(cpu,20)
    def _network(self,cpu,address,size,user):self._return(cpu,0)
    def prepare_surface_world(self,case,body):
        super().prepare_surface_world(case,body)
        self.prepare_queue(case,body)
        self.write(self.globals+0x12c,'I',case['idle_time'])
        self.write(self.globals+0x13c,'I',case['heartbeat_time'])
        self.write(self.passenger+0x10,'I',0x200 if case['defer'] else 0)
        self.write(self.unit+0xd8,'I',body)
        # Original SendMovementUpdate uses the unit's inline mover only for
        # UpdateHeartbeatTimerA; supply its unit back-reference as well.
        self.write(self.unit+0x788+0x144,'I',self.unit)
        table=self.alloc(bytes(0x200));self.write(self.unit,'I',table)
        for slot,offset in [(0x38,0x20),(0x14c,0x24)]:
            # External virtual property getters: fld dword [body+offset]; ret.
            getter=self.alloc_code(b'\xd9\x05'+struct.pack('<I',body+offset)+b'\xc3')
            self.write(table+slot,'I',getter)
        for op in case['operations']:
            if op[0]!=0:raise ValueError('Execution fixture accepts enqueues only')
            _,time,height,counter,ack=op;bits=struct.unpack('<I',struct.pack('<f',height))[0]
            self.call(0x6ee760 if ack else 0x6ee7b0,[time,counter,bits] if ack else [time,bits],body)
    def run(self,case):
        self.requests=[];self.events=[];self.geometry_status=0;self.consumed=0
        self.notifications=[];self.heartbeats=[];self.finished=[];self.retire=False
        result=self.surface_move(dict(case,triangles=[],liquids=[]),routine=0x6f09f0)
        result['elapsed']=self.consumed
        return dict(**result,requests=self.requests,events=self.events,geometry_status=self.geometry_status,
            notifications=self.notifications,heartbeats=self.heartbeats,finished=self.finished,retire=self.retire,
            height=self.read(self.body+0xcc,'f')[0],flags2=self.read(self.body+0x48,'H')[0],
            heartbeat_time=self.read(self.globals+0x13c,'I')[0],idle_time=self.read(self.globals+0x12c,'I')[0])
