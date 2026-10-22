#!/usr/bin/env python3
"""Execute 12340's DistanceToMove in an isolated x86 emulator, without an OS.

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
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
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

    def alloc(self, blob):
        p = self.next
        self.next += (len(blob) + 15) & ~15
        if self.next >= 0x2400000:
            raise ValueError('Reference fixture exceeds arena')
        self.cpu.mem_write(p, blob)
        return p

    def floats(self, values):
        return self.alloc(struct.pack('<' + 'f' * len(values), *values))

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
            print(json.dumps(original.distance_to_move(json.loads(line))), flush=True)


if __name__ == '__main__':
    main()
