#!/usr/bin/env python3
"""Original server mode queue with synchronous unit/input boundary observations."""
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP
from original_movement_rate_queue_reference import OriginalMovementRateQueue

class OriginalMovementModeQueue(OriginalMovementRateQueue):
    def __init__(self,binary):
        super().__init__(binary)
        for address,callback in [(0x73ad00,self._falling),(0x71afe0,self._unroot),
                                  (0x5f95f0,self._move_stopped),(0x73d3d0,self._fall_land)]:
            self.cpu.hook_add(UC_HOOK_CODE,callback,begin=address,end=address)
    def observe(self,kind,time=0,flags=0,flags2=0,jumping=False,opcode=0):
        self.mode_observations.append([kind,time,flags,flags2,int(jumping),opcode,
            self.read(self.body+0x44,'I')[0],self.read(self.body+0x48,'H')[0],
            self.read(self.body+0x80,'I')[0],self.read(self.body+0xb8,'f')[0],
            self.read(self.body+0x8c,'f')[0],self.read(self.body+0x60,'I')[0]])
    def _falling(self,cpu,address,size,user):self.observe(1);self._return(cpu,0)
    def _unroot(self,cpu,address,size,user):self.observe(2);self._return(cpu,0)
    def _move_stopped(self,cpu,address,size,user):self.observe(3);self._return(cpu,0)
    def _fall_land(self,cpu,address,size,user):
        flags,jumping=self.read(cpu.reg_read(UC_X86_REG_ESP)+4,'2I')
        self.observe(4,flags=flags,jumping=jumping);self._return(cpu,8)
    def _retire(self,cpu,address,size,user):
        if self.read(cpu.reg_read(UC_X86_REG_ESP),'I')[0]==0x6ebb0a:self.observe(5)
        super()._retire(cpu,address,size,user)
    def _events(self,cpu,address,size,user):
        ret,time,left,flags,flags2,jumping,transport=self.read(cpu.reg_read(UC_X86_REG_ESP),'7I')
        if 0x6ef860<=ret<0x6f082a:
            if left or transport:raise RuntimeError('Unexpected queued state effect')
            self.observe(6,time,flags,flags2,jumping);self._return(cpu,24)
        else:super()._events(cpu,address,size,user)
    def observe_local_event(self,cpu,address,size,user):
        time,opcode=self.read(cpu.reg_read(UC_X86_REG_ESP)+4,'2I')
        self.observe(0,time,opcode=opcode)
        if opcode in (0x4d1,0x4cf,0x2cf,0x2d0,0xf6,0xe9,0xeb,0x345,0x340):
            self.rate_states.append(self.rate_state())
        super().observe_local_event(cpu,address,size,user)
    def run(self,case):
        self.mode_observations=[]
        output=super().run(case)
        output['mode_observations']=self.mode_observations
        return output
