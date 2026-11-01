#!/usr/bin/env python3
"""Original queue, collision frame and real local-event reporting composition.

Unit/input effects, lifecycle and collision event callbacks remain observed
boundaries. The original local notification, send admission, serialization and
heartbeat updates execute; no successful packet-preparation stub is used.
"""
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP
from original_movement_mode_queue_reference import OriginalMovementModeQueue
from original_movement_reporting_reference import OriginalMovementReporting
from original_movement_local_event_reference import OriginalMovementLocalEvent

class OriginalMovementDelivery(OriginalMovementModeQueue):
    ret = OriginalMovementReporting.ret
    _network = OriginalMovementReporting._network
    _free = OriginalMovementReporting._free
    _prepare_packet = OriginalMovementReporting._storage
    _effect = OriginalMovementLocalEvent._effect

    def __init__(self, binary):
        super().__init__(binary)
        self.cpu.hook_add(UC_HOOK_CODE, self._free, begin=0x47ae50, end=0x47ae50)
        for address in [0x721300, 0x6dcb40, 0x5fbbc0, 0x71b1e0, 0x73ed10]:
            self.cpu.hook_add(UC_HOOK_CODE, self._effect, begin=address, end=address)

    def _local_event(self, cpu, address, size, user):
        # Record without redirecting EIP: restoring EIP to this hook's address
        # after a stub return would re-enter the hook instead of running code.
        self.observe_local_event(cpu,address,size,user)

    def _send(self, cpu, address, size, user):
        opcode = self.read(cpu.reg_read(UC_X86_REG_ESP) + 8, 'I')[0]
        if opcode == 0xee:
            super()._send(cpu, address, size, user)

    def prepare_surface_world(self, case, body):
        super().prepare_surface_world(case, body)
        if body != self.unit + 0x788:
            raise RuntimeError('Delivery fixture needs the actual inline mover')
        initial=case.get('reporting',{})
        self.write(self.unit + 0xa50, '2f', initial.get('reported_facing',self.read(body+0x20,'f')[0]),
                   initial.get('reported_pitch',self.read(body+0x24,'f')[0]))
        self.write(self.unit + 0xa30, 'I', initial.get('unit_flags',0x400 if case.get('local',False) else 0))
        self.write(self.unit + 0x948, '4I', initial.get('turn_time',0),initial.get('pitch_time',0),
                   initial.get('pending_turn',0),initial.get('pending_pitch',0))
        table = self.read(self.unit, 'I')[0]
        getter = self.alloc_code(b'\x31\xc0\xc3')
        self.write(table + 0x138, 'I', getter)  # supplied standing-state property

    def run(self, case):
        self.packets = []
        self.at_send = []
        self.effects = []
        output = super().run(dict(case, inline_mover=True))
        output['reporting'] = dict(packets=self.packets, at_send=self.at_send, effects=self.effects,
            unit_flags=self.read(self.unit + 0xa30, 'I')[0],
            reported_facing=self.read(self.unit + 0xa50, 'f')[0],
            reported_pitch=self.read(self.unit + 0xa54, 'f')[0],
            turn_time=self.read(self.unit + 0x948, 'I')[0], pitch_time=self.read(self.unit + 0x94c, 'I')[0],
            pending_turn=self.read(self.unit + 0x950, 'I')[0], pending_pitch=self.read(self.unit + 0x954, 'I')[0])
        return output
