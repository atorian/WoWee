#!/usr/bin/env python3
"""Execute original shared speed/rate and movement-mode transitions, no spline."""
import struct
from original_movement_input_reference import OriginalMovementInput

class OriginalMovementState(OriginalMovementInput):
    def run(self, case):
        self.surface_move(dict(case, elapsed=0), routine=0x6eac40)
        command = case['command']
        addresses = [0x98a550, 0x98a0b0, 0x98a300, 0x98a7a0, 0x98a9f0,
                     0x98ac40, 0x98ae90, 0x989c50, 0x989e80, 0x98b310,
                     0x987440, 0x987410, 0x9873f0, 0x987460, 0x98c4f0, 0x98c550, 0x98bff0, 0x98c240, 0x988490, 0x98b710]
        value = struct.unpack('<I', struct.pack('<f', case['value']))[0] if command < 9 else int(bool(case['value']))
        args = [] if command in (11, 14, 16, 17, 18, 19) else [value]
        changed = self.call(addresses[command], args, self.body)
        return self.snapshot(case, bool(changed) if command < 10 or command == 19 else True)

    def snapshot(self, case, accepted=True):
        state = self.surface_state(case, self.body)
        state['elapsed'] = 0
        return dict(**state, accepted=accepted,
                    flags2=int.from_bytes(self.cpu.mem_read(self.body + 0x48, 2), 'little'),
                    speeds=list(struct.unpack('<7f', self.cpu.mem_read(self.body + 0x90, 28))),
                    turn_rate=struct.unpack('<f', self.cpu.mem_read(self.body + 0xac, 4))[0],
                    pitch_rate=struct.unpack('<f', self.cpu.mem_read(self.body + 0xb0, 4))[0],
                    requests=[], events=[], geometry_status=0)
