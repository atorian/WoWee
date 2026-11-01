#!/usr/bin/env python3
"""Original queued basic inputs, with observed unit and input-control delivery."""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX
from original_movement_execution_reference import OriginalMovementExecution

class OriginalMovementInputQueue(OriginalMovementExecution):
    def __init__(self,binary):
        super().__init__(binary)
        for address,callback in [(0x5f95d0,self._input_control),(0x5f9650,self._stop_turn),(0x5fb510,self._stop_pitch)]:
            self.cpu.hook_add(UC_HOOK_CODE,callback,begin=address,end=address)
    def _input_control(self,cpu,address,size,user):
        cpu.reg_write(UC_X86_REG_EAX,self.input_control);self._return(cpu,0)
    def _stop_turn(self,cpu,address,size,user):self.angular_stops.append(0);self._return(cpu,0)
    def _stop_pitch(self,cpu,address,size,user):self.angular_stops.append(1);self._return(cpu,0)
    def _local_event(self,cpu,address,size,user):
        super()._local_event(cpu,address,size,user)
        # Delivery is observed and reports no packet sent. Original dispatch
        # still executes its own fallback UpdateHeartbeatTimerA when needed.
        cpu.reg_write(UC_X86_REG_EAX,0)
    def prepare_surface_world(self,case,body):
        super().prepare_surface_world(dict(case,operations=[]),body)
        self.input_control=self.alloc(bytes(0x100))
        ids=[0,1,2,3,4,5,11,12,13,14,15,16,6,7,8,10]
        for op in case['inputs']:
            if isinstance(op,dict):
                mode=op['mode'];enable=op['enabled']
                addresses=[0x6ee690,0x6eee50,0x6eed70,0x6eeef0,0x6eeff0,0x6ef060,0x6ef150]
                self.call(addresses[mode],[op['time'],op['counter'],int(not enable) if mode==0 else int(enable)],body)
            elif len(op)==6:
                time,x,y,speed,vertical,counter=op
                bits=struct.unpack('<2I',struct.pack('<2f',speed,vertical))
                self.call(0x6ecc80,[time,counter,self.floats([x,y]),*bits],body)
            elif len(op)==5:
                time,rate,value,counter,ack=op
                local=[0x6edd10,0x6edb30,0x6edc20,0x6ede00,0x6edef0,0x6edfe0,0x6ee0d0,0x6ee1c0,0x6ee2b0]
                bits=struct.unpack('<I',struct.pack('<f',value))[0]
                self.call(local[rate] if ack else local[rate]+0x50,
                    [time,counter,bits] if ack else [time,bits],body)
            elif len(op)==4:
                time,height,counter,ack=op;bits=struct.unpack('<I',struct.pack('<f',height))[0]
                self.call(0x6ee760 if ack else 0x6ee7b0,[time,counter,bits] if ack else [time,bits],body)
            else:
                time,command,ack=op
                self.call(0x6ec840,[time,ids[command],ack,0,0,0,0],body)
    def run(self,case):
        self.requests=[];self.events=[];self.geometry_status=0;self.consumed=0
        self.notifications=[];self.heartbeats=[];self.finished=[];self.retire=False;self.angular_stops=[]
        self.surface_move(dict(case,triangles=[],liquids=[]),routine=0x6f09f0 if case.get("execute_input",False) else 0x6ef860)
        queue_result=self.cpu.reg_read(UC_X86_REG_EAX)
        result=self.surface_state(case,self.body);result['elapsed']=self.consumed if case.get('execute_input',False) else 0
        output=dict(**result,requests=self.requests,events=self.events,geometry_status=self.geometry_status,
            notifications=self.notifications,heartbeats=self.heartbeats,finished=self.finished,retire=self.retire,
            height=self.read(self.body+0xcc,'f')[0],flags2=self.read(self.body+0x48,'H')[0],
            heartbeat_time=self.read(self.globals+0x13c,'I')[0],idle_time=self.read(self.globals+0x12c,'I')[0],
            angular_stops=self.angular_stops)
        if not case.get("execute_input",False):output["queue_result"]=queue_result
        return output
