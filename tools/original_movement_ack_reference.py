#!/usr/bin/env python3
"""Execute build 12340 PrepareMovementPacket, including its real status writer."""
import struct
from original_movement_status_reference import OriginalMovementStatus

class OriginalMovementAck(OriginalMovementStatus):
    def run(self, case):
        self.surface_move(dict(case, elapsed=0), routine=0x6eac40)
        unit = struct.unpack('<I', self.cpu.mem_read(self.body + 0x144, 4))[0]
        inline = unit + 0x788
        self.cpu.mem_write(inline, bytes(self.cpu.mem_read(self.body, 0x148)))
        self.cpu.mem_write(unit + 0xd8, struct.pack('<I', inline))
        fields = struct.unpack('<I', self.cpu.mem_read(unit + 8, 4))[0]
        guid = struct.pack('<Q', case['guid'])
        self.cpu.mem_write(fields, guid)
        self.cpu.mem_write(0xca1238, guid)
        buffer = self.alloc(bytes(1024))
        store = self.alloc(struct.pack('<6I', 0x9e0e24, buffer, 0, 1024, 0, 0xffffffff))
        value = struct.unpack('<I', struct.pack('<f', case['value']))[0]
        accepted = self.call(0x71ef80, [case['time'], case['opcode'], store, value, case['counter']], unit)
        if not accepted:
            raise RuntimeError('Original rejected fixture movement ACK')
        length = struct.unpack('<I', self.cpu.mem_read(store + 16, 4))[0]
        if length > 1024:
            raise RuntimeError('ACK exceeded fixture storage')
        data = bytes(self.cpu.mem_read(buffer, length))
        return dict(name=case['name'], opcode=struct.unpack('<I', data[:4])[0], body=data[4:].hex())
