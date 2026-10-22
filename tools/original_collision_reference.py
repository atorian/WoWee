#!/usr/bin/env python3
"""Execute 12340 collision and surface movement in an isolated x86 emulator.

Requires pefile and unicorn in the caller's Python environment. The caller
supplies their original executable; no proprietary bytes are distributed.
Input/output are JSON lines. This is an offline reference, never a game path.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UcError
from unicorn.x86_const import (UC_X86_REG_ECX, UC_X86_REG_ESP,
                              UC_X86_REG_EIP, UC_X86_REG_EAX,
                              UC_X86_REG_FPCW, UC_X86_REG_FPSW,
                              UC_X86_REG_FPTAG)

REFERENCE_SHA256 = '0302a63eda5d63150f6cdb5701c5d15ee26aa3a667a8e6febb688b46707f9b81'


class OriginalCollision:
    def __init__(self, binary, control_word=0x27f):
        data = Path(binary).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != REFERENCE_SHA256:
            raise ValueError(f'Unsupported reference binary SHA-256: {digest}')
        pe = pefile.PE(data=data)
        if pe.OPTIONAL_HEADER.ImageBase != 0x400000:
            raise ValueError('Reference image base differs')
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_32)
        size = (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095
        self.cpu.mem_map(0x400000, size)
        self.cpu.mem_write(0x400000, pe.get_memory_mapped_image())
        self.cpu.mem_map(0x2000000, 0x400000)
        self.cpu.mem_map(0x3000000, 0x100000)
        self.cpu.mem_map(0x1000000, 0x1000)
        self.control_word = control_word
        self.transport_request = None
        self.fall_reset = None
        self.cpu.hook_add(UC_HOOK_CODE, self._observe_fall_reset, begin=0x6e9270, end=0x6e9270)
        self.cpu.hook_add(UC_HOOK_CODE, self._observe_transport, begin=0x6ec7b0, end=0x6ec7b0)

    def _observe_fall_reset(self, cpu, address, size, user_data):
        stack = cpu.reg_read(UC_X86_REG_ESP)
        self.fall_reset = struct.unpack('<I', cpu.mem_read(stack + 4, 4))[0]

    def ground_move(self, case):
        return self.surface_move(case, routine=0x75ee60)

    def hover_move(self, case):
        return self.surface_move(case, routine=0x762980)

    def swim_move(self, case):
        return self.surface_move(case, routine=0x760b40)

    def fall_move(self, case):
        return self.surface_move(case, routine=0x7618b0)

    def _observe_transport(self, cpu, address, size, user_data):
        # Observe the request at the API boundary; execute the real function.
        stack = cpu.reg_read(UC_X86_REG_ESP)
        self.transport_request = struct.unpack('<Q', cpu.mem_read(stack + 4, 8))[0]

    def alloc(self, blob):
        p = self.next
        self.next += (len(blob) + 15) & ~15
        if self.next >= 0x2400000:
            raise ValueError('Reference fixture exceeds arena')
        self.cpu.mem_write(p, blob)
        return p

    def floats(self, values):
        return self.alloc(struct.pack('<' + 'f' * len(values), *values))

    def call(self, address, args, this=0):
        cpu = self.cpu
        stack = 0x30ff000
        cpu.mem_write(stack, struct.pack('<'+'I'*(len(args)+1), 0x1000000, *args))
        cpu.reg_write(UC_X86_REG_ESP, stack)
        cpu.reg_write(UC_X86_REG_ECX, this)
        cpu.reg_write(UC_X86_REG_FPCW, self.control_word)
        cpu.reg_write(UC_X86_REG_FPSW, 0)
        cpu.reg_write(UC_X86_REG_FPTAG, 0xffff)
        try:
            cpu.emu_start(address, 0x1000000, count=10_000_000)
        except UcError as error:
            raise RuntimeError(f'Original routine failed at {cpu.reg_read(UC_X86_REG_EIP):08x}: {error}') from error
        if cpu.reg_read(UC_X86_REG_EIP) != 0x1000000:
            raise RuntimeError('Original routine exceeded instruction budget')
        return cpu.reg_read(UC_X86_REG_EAX)

    def surface_move(self, case, routine=0x7620f0):
        """Run TraceSurface with a real player-shaped unit and preloaded world.

        No collision, stepping, unit-control, falling or arithmetic function
        is replaced. All facets are already resident inside a large query box;
        this fixture does not model world streaming or moving transports.
        """
        self.next = 0x2000000
        cpu = self.cpu
        self.transport_request = None
        self.fall_reset = None
        body = self.alloc(bytes(0x500))
        unit = self.alloc(bytes(0x1000))
        fields = self.alloc(bytes(0x1000))
        object_fields = self.alloc(struct.pack('<QII', 1, 0x10 if case.get('client_controlled', True) else 0x8, 0))
        def write(address, fmt, *values):
            cpu.mem_write(address, struct.pack('<' + fmt, *values))
        write(unit + 8, 'I', object_fields)
        write(unit + 0xd0, 'I', fields)
        write(body + 0x144, 'I', unit)
        write(body + 0x28, 'I', object_fields)
        write(body + 0x10, '3f', *case['position'])
        write(body + 0x20, 'f', case.get('facing', 0))
        write(body + 0x24, 'f', case.get('pitch', 0))
        write(body + 0x48, 'H', case.get('flags2', 0))
        write(body + 0x4c, '3f', *case.get('anchor_position', [0, 0, 0]))
        write(body + 0x58, '2f', case.get('anchor_facing', 0), case.get('anchor_pitch', 0))
        write(body + 0x60, 'I', case.get('anchor_elapsed', 0))
        write(body + 0x64, '3f', *case.get('move_direction', [0, 0, 0]))
        write(body + 0x70, '2f', *case.get('move_direction_2d', [0, 0]))
        write(body + 0x78, '2f', case.get('pitch_cos', 1), case.get('pitch_sin', 0))
        write(body + 0x80, 'I', case.get('fall_time', 0))
        write(body + 0x84, 'f', case.get('fall_start', 0))
        write(body + 0x8c, 'f', case.get('current_speed', 0))
        write(body + 0xb4, 'f', case.get('hover_height', 0))
        write(body + 0xb8, 'f', case.get('fall_velocity', 0))
        write(body + 0x38, '3f', *case.get('ground_normal', [0, 0, 0]))
        write(body + 0x90, '7f', *case.get('speeds', [0]*7))
        write(body + 0x44, 'I', case.get('flags', 1))
        write(body + 0x88, 'f', case.get('step_start', case['position'][2]))
        write(body + 0xc8, '3f', case['radius'], case['height'], case.get('step_height', 1))
        triangles = case['triangles']
        data = b''.join(struct.pack('<13f', *(t['plane'] + sum(t['vertices'], []))) for t in triangles)
        facets = self.alloc(data or bytes(16))
        ids = self.alloc(b''.join(struct.pack('<Q', t.get('owner', 0)) for t in triangles) or bytes(8))
        write(0xadba34, '8I', len(triangles), len(triangles), facets, 0,
              len(triangles), len(triangles), ids, 0)
        liquid = case.get('liquids', [])
        liquid_data = self.alloc(b''.join(struct.pack('<13f', *(t['plane'] + sum(t['vertices'], []))) for t in liquid) or bytes(16))
        liquid_ids = self.alloc(b''.join(struct.pack('<Q', t.get('owner', 0)) for t in liquid) or bytes(8))
        write(0xadba54, '8I', len(liquid), len(liquid), liquid_data, 0, len(liquid), len(liquid), liquid_ids, 0)
        write(0xca1660, '6f', -100000, -100000, -100000, 100000, 100000, 100000)
        direction = self.floats(case['direction'][:2])
        distance_bits = struct.unpack('<I', struct.pack('<f', case['distance']))[0]
        stack = 0x30ff000
        write(stack, '5I', 0x1000000, case.get('time', 1000), case.get('elapsed', 16), distance_bits, direction)
        if routine in (0x760b40, 0x762980):
            write(stack, '4I3f', 0x1000000, case.get('time', 1000), case.get('elapsed', 16), distance_bits, *case['swim_direction'])
        cpu.reg_write(UC_X86_REG_ESP, stack)
        cpu.reg_write(UC_X86_REG_ECX, body)
        cpu.reg_write(UC_X86_REG_FPCW, self.control_word)
        cpu.reg_write(UC_X86_REG_FPSW, 0)
        cpu.reg_write(UC_X86_REG_FPTAG, 0xffff)
        try:
            cpu.emu_start(routine, 0x1000000, count=10_000_000)
        except UcError as error:
            raise RuntimeError(f'Original surface failed at EIP={cpu.reg_read(UC_X86_REG_EIP):08x}: {error}') from error
        if cpu.reg_read(UC_X86_REG_EIP) != 0x1000000:
            raise RuntimeError('Original surface exceeded its instruction budget')
        result = dict(name=case.get('name', ''),
                    position=list(struct.unpack('<3f', cpu.mem_read(body + 0x10, 12))),
                    flags=struct.unpack('<I', cpu.mem_read(body + 0x44, 4))[0],
                    step_start=struct.unpack('<f', cpu.mem_read(body + 0x88, 4))[0],
                    elapsed=0 if routine == 0x75ee60 else cpu.reg_read(UC_X86_REG_EAX))
        for name, offset, fmt in [('fall_time', 0x80, 'I'), ('fall_start', 0x84, 'f'),
                                  ('fall_velocity', 0xb8, 'f'), ('current_speed', 0x8c, 'f'),
                                  ('anchor_elapsed', 0x60, 'I'), ('anchor_facing', 0x58, 'f'),
                                  ('anchor_pitch', 0x5c, 'f'), ('anchor_position', 0x4c, '3f'),
                                  ('move_direction', 0x64, '3f'), ('move_direction_2d', 0x70, '2f')]:
            values = struct.unpack('<' + fmt, cpu.mem_read(body + offset, struct.calcsize(fmt)))
            result[name] = values[0] if len(values) == 1 else list(values)
        result['transport'] = self.transport_request
        if routine in (0x7618b0, 0x760b40, 0x762980, 0x75ee60):
            result['ground_normal'] = list(struct.unpack('<3f', cpu.mem_read(body + 0x38, 12)))
            result['fall_reset'] = self.fall_reset
        return result

    def distance_to_move(self, case):
        self.next = 0x2000000
        cpu = self.cpu
        body = self.alloc(bytes(0x500))
        cpu.mem_write(body + 0xc8, struct.pack('<ff', case['radius'], case['height']))
        triangles = case['triangles']
        packed = bytearray()
        for tri in triangles:
            vertices = tri['vertices']
            plane = tri['plane']
            packed.extend(struct.pack('<13f', *(plane + sum(vertices, []))))
        array = self.alloc(bytes(packed) or bytes(16))
        facets = self.alloc(struct.pack('<IIII', len(triangles), len(triangles), array, 0) + bytes(32))
        position = self.floats(case['position'])
        direction = self.floats(case['direction'])
        index = self.alloc(struct.pack('<I', 0xffffffff))
        planes = self.alloc(bytes(16 * 16))
        count = self.alloc(bytes(4))
        distance = self.alloc(bytes(4))
        distance_bits = struct.unpack('<I', struct.pack('<f', case['distance']))[0]
        args = [facets, direction, distance_bits, index, planes, count, distance, position]
        stack = 0x30ff000
        cpu.mem_write(stack, struct.pack('<9I', 0x1000000, *args))
        cpu.reg_write(UC_X86_REG_ESP, stack)
        cpu.reg_write(UC_X86_REG_ECX, body)
        cpu.reg_write(UC_X86_REG_FPCW, self.control_word)
        cpu.reg_write(UC_X86_REG_FPSW, 0)
        cpu.reg_write(UC_X86_REG_FPTAG, 0xffff)
        try:
            cpu.emu_start(0x75f9d0, 0x1000000, count=5_000_000)
        except UcError as error:
            raise RuntimeError(f'Original failed at EIP={cpu.reg_read(UC_X86_REG_EIP):08x}: {error}') from error
        if cpu.reg_read(UC_X86_REG_EIP) != 0x1000000 or cpu.reg_read(UC_X86_REG_EAX) != 1:
            raise RuntimeError('Original did not return a successful result')
        plane_count = struct.unpack('<I', cpu.mem_read(count, 4))[0]
        if plane_count > 9:
            raise RuntimeError(f'Invalid original contact count: {plane_count}')
        return {'name': case.get('name', ''),
                'distance': struct.unpack('<f', cpu.mem_read(distance, 4))[0],
                'facet': struct.unpack('<I', cpu.mem_read(index, 4))[0],
                'planes': [list(struct.unpack('<4f', cpu.mem_read(planes + i * 16, 16)))
                           for i in range(plane_count)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--fpu-control', type=lambda v: int(v, 0), default=0x27f)
    args = parser.parse_args()
    original = OriginalCollision(args.binary, args.fpu_control)
    for line in sys.stdin:
        if line.strip():
            case = json.loads(line)
            result = original.surface_move(case) if case.get('operation') == 'surface' else original.distance_to_move(case)
            print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
