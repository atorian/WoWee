#!/usr/bin/env python3
"""Execute original SendMovementUpdate, admission, status, writers and clock.

External boundaries: virtual facing/pitch getters read fixture state; the
network captures bytes; thread globals and CDataStore storage are supplied.
No packet preparation, admission, writer or heartbeat function is replaced.
"""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX, UC_X86_REG_EIP
from original_movement_status_reference import OriginalMovementStatus

class OriginalMovementReporting(OriginalMovementStatus):
    def __init__(self, binary):
        super().__init__(binary)
        for address, callback in [(0x71ef80, self._storage), (0x6b0b50, self._network),
                                  (0x74b330, self._globals), (0x47ae50, self._free)]:
            self.cpu.hook_add(UC_HOOK_CODE, callback, begin=address, end=address)

    def read(self, address, fmt):
        return struct.unpack('<' + fmt, self.cpu.mem_read(address, struct.calcsize('<' + fmt)))

    def write(self, address, fmt, *values):
        self.cpu.mem_write(address, struct.pack('<' + fmt, *values))

    def ret(self, count=0):
        sp = self.cpu.reg_read(UC_X86_REG_ESP)
        self.cpu.reg_write(UC_X86_REG_EIP, self.read(sp, 'I')[0])
        self.cpu.reg_write(UC_X86_REG_ESP, sp + 4 + count)

    def _storage(self, cpu, address, size, user):
        store = self.read(cpu.reg_read(UC_X86_REG_ESP) + 12, 'I')[0]
        buffer = self.alloc(bytes(1024))
        self.write(store, '6I', 0x9e0e24, buffer, 0, 1024, 0, 0xffffffff)

    def _free(self, cpu, address, size, user):
        # Only allocation ownership is external; arena storage lives to case end.
        self.ret(12)

    def _globals(self, cpu, address, size, user):
        cpu.reg_write(UC_X86_REG_EAX, self.globals)
        self.ret()

    def _network(self, cpu, address, size, user):
        store = self.read(cpu.reg_read(UC_X86_REG_ESP) + 4, 'I')[0]
        _, buffer, _, capacity, length, _ = self.read(store, '6I')
        if not 4 <= length <= capacity == 1024:
            raise RuntimeError('Unexpected original packet storage')
        data = bytes(cpu.mem_read(buffer, length))
        self.packets.append(dict(opcode=struct.unpack('<I', data[:4])[0], body=data[4:].hex()))
        # Observe exactly when reporting state is visible to the connection.
        self.at_send.append([self.read(self.unit + 0xa30, 'I')[0],
                             self.read(self.globals + 0x13c, 'I')[0]])
        self.ret()

    def prepare(self, case):
        self.surface_move(dict(case, elapsed=0), routine=0x6eac40)
        self.unit = self.read(self.body + 0x144, 'I')[0]
        inline = self.unit + 0x788
        self.cpu.mem_write(inline, bytes(self.cpu.mem_read(self.body, 0x148)))
        self.write(self.unit + 0xd8, 'I', inline)
        fields = self.read(self.unit + 8, 'I')[0]
        self.write(fields, 'Q', case['guid'])
        self.write(0xca1238, 'Q', case['guid'] if case['active'] else case['guid'] ^ 1)
        self.globals = self.alloc(bytes(0x200))
        self.write(self.globals + 0x13c, 'I', case['heartbeat_time'])
        self.write(self.unit + 0xa30, 'I', case['unit_flags'])
        self.write(self.unit + 0xa50, '2f', case['reported_facing'], case['reported_pitch'])
        table = self.alloc(bytes(0x200))
        self.write(self.unit, 'I', table)
        for slot, offset in [(0x38, 0x20), (0x14c, 0x24)]:
            getter = self.alloc_code(b'\xd9\x05' + struct.pack('<I', inline + offset) + b'\xc3')
            self.write(table + slot, 'I', getter)
        self.packets = []
        self.at_send = []

    def run(self, case):
        self.prepare(case)
        value = struct.unpack('<I', struct.pack('<f', case['value']))[0]
        accepted = self.call(0x71f0c0, [case['time'], case['opcode'], value,
                                      case['counter'], 0, 0, 255], self.unit)
        return self.result(case, accepted)

    def result(self, case, accepted):
        return dict(name=case['name'], accepted=bool(accepted), packets=self.packets,
                    at_send=self.at_send, unit_flags=self.read(self.unit + 0xa30, 'I')[0],
                    heartbeat_time=self.read(self.globals + 0x13c, 'I')[0],
                    reported_facing=self.read(self.unit + 0xa50, 'f')[0],
                    reported_pitch=self.read(self.unit + 0xa54, 'f')[0])
