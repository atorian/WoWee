#!/usr/bin/env python3
"""Execute the original unattached Launch, including swim/flight exit."""
import struct
from original_movement_state_reference import OriginalMovementState

class OriginalMovementLaunch(OriginalMovementState):
    def run(self,case):
        self.surface_move(dict(case,elapsed=0),routine=0x6eac40)
        direction=self.floats(case['launch_direction'])
        speed,vertical=struct.unpack('<2I',struct.pack('<2f',case['launch_speed'],case['launch_vertical']))
        self.call(0x6e9ff0,[direction,speed,vertical],self.body)
        return self.snapshot(case)
