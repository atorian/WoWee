#!/usr/bin/env python3
"""Original timed rate commands; record state visible at each unit notification."""
from unicorn.x86_const import UC_X86_REG_ESP
from original_movement_input_queue_reference import OriginalMovementInputQueue

class OriginalMovementRateQueue(OriginalMovementInputQueue):
    def rates(self):
        return dict(speeds=list(self.read(self.body+0x90, '7f')),
                    turn_rate=self.read(self.body+0xac, 'f')[0], pitch_rate=self.read(self.body+0xb0, 'f')[0])
    def rate_state(self):
        return dict(**self.rates(), flags=self.read(self.body+0x44, 'I')[0],
                flags2=self.read(self.body+0x48, 'H')[0], current_speed=self.read(self.body+0x8c, 'f')[0],
                fall_time=self.read(self.body+0x80, 'I')[0], fall_velocity=self.read(self.body+0xb8, 'f')[0],
                anchor_elapsed=self.read(self.body+0x60, 'I')[0])
    def _local_event(self, cpu, address, size, user):
        opcode=self.read(cpu.reg_read(UC_X86_REG_ESP)+8, 'I')[0]
        if opcode in (0xf0,0x2db,0xe3,0xe5,0xe7,0x2dd,0x382,0x384,0x2df,0x45d):
            self.rate_states.append(self.rate_state())
        super()._local_event(cpu,address,size,user)
    def run(self,case):
        self.rate_states=[]
        output=super().run(case)
        output.update(self.rates(),rate_states=self.rate_states)
        return output
