#!/usr/bin/env python3
"""Original local notification and delayed angular reporting with actual packets.

Unit presentation/input callbacks are recorded, not applied. Their order is
observable; their effect on real input control remains an integration boundary.
"""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX
from original_movement_reporting_reference import OriginalMovementReporting

class OriginalMovementLocalEvent(OriginalMovementReporting):
    def __init__(self, binary):
        super().__init__(binary)
        for address in [0x721300, 0x6dcb40, 0x5fbbc0, 0x71b1e0, 0x73ed10]:
            self.cpu.hook_add(UC_HOOK_CODE, self._effect, begin=address, end=address)
        self.cpu.hook_add(UC_HOOK_CODE, self._input, begin=0x5f95d0, end=0x5f95d0)

    def _input(self, cpu, address, size, user):
        cpu.reg_write(UC_X86_REG_EAX, self.input_control)
        self.ret()

    def _effect(self, cpu, address, size, user):
        kind, count = {0x721300: (1, 0), 0x6dcb40: (2, 4), 0x5fbbc0: (3, 8),
                       0x71b1e0: (4, 0), 0x73ed10: (5, 4)}[address]
        args = list(self.read(cpu.reg_read(UC_X86_REG_ESP) + 4, 'I' * (count // 4)))
        self.effects.append([kind, *args, len(self.packets), self.read(self.unit + 0xa30, 'I')[0],
                             self.read(self.globals + 0x13c, 'I')[0]])
        self.ret(count)

    def run(self, case):
        self.prepare(case)
        self.effects = []
        self.input_control = self.alloc(bytes(0x200))
        table = self.read(self.unit, 'I')[0]
        stand = self.alloc(struct.pack('<I', case['stand_state']))
        getter = self.alloc_code(b'\xa1' + struct.pack('<I', stand) + b'\xc3')
        self.write(table + 0x138, 'I', getter)
        fields = self.read(self.unit + 8, 'I')[0]
        self.write(fields + 8, 'I', 0x10 if case['player'] else 0x8)
        self.write(self.unit + 0x948, '4I', case['turn_time'], case['pitch_time'],
                   case['pending_turn'], case['pending_pitch'])
        value = struct.unpack('<I', struct.pack('<f', case['value']))[0]
        if case.get('flush', False):
            self.call(0x721b90, [case['time']], self.unit)
            accepted = bool(self.packets)
        else:
            accepted = self.call(0x7413f0, [case['time'], case['opcode'], int(case['need_ack']),
                                          value, case['counter'], 0, 0, 255], self.unit)
        return dict(**self.result(case, accepted), effects=self.effects,
                    turn_time=self.read(self.unit + 0x948, 'I')[0],
                    pitch_time=self.read(self.unit + 0x94c, 'I')[0],
                    pending_turn=self.read(self.unit + 0x950, 'I')[0],
                    pending_pitch=self.read(self.unit + 0x954, 'I')[0])
