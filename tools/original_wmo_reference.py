#!/usr/bin/env python3
"""Execute build 12340's actual WMO BSP query on supplied model-local data."""
import argparse
import json
import struct
import sys
from original_collision_reference import OriginalCollision



class OriginalWmoQuery(OriginalCollision):
    def __init__(self, binary):
        super().__init__(binary)
        # CAaBspDigestCache has 1024 entries, each 0x2460 bytes. The original
        # code constructs digests itself; no cached-query routine is replaced.
        self.cache = 0x4000000
        self.cpu.mem_map(self.cache, 0x1000000)

    def query(self, case):
        self.next = 0x2000000
        cpu = self.cpu
        def write(address, fmt, *values):
            cpu.mem_write(address, struct.pack('<'+fmt, *values))
        nodes = self.alloc(b''.join(struct.pack('<4HIf', *n) for n in case['nodes']))
        faces = self.alloc(struct.pack('<'+'H'*len(case['faces']), *case['faces']))
        vertices = self.floats(sum(case['vertices'], []))
        indices = sum(case['triangles'], [])
        indices_ptr = self.alloc(struct.pack('<'+'H'*len(indices), *indices))
        flags = case.get('triangle_flags', [0]*len(case['triangles']))
        polygons = self.alloc(b''.join(bytes([f, 0]) for f in flags))
        query_box = self.floats(case['box'])
        bounds = self.floats(case['bounds'])
        bsp = self.alloc(bytes(0x68))
        write(bsp, '5I', nodes, nodes, faces, len(case['nodes']), len(case['faces']))
        write(bsp + 0x4c, '6f', *case['bounds'])
        overflow = self.alloc(bytes(4))
        ignore = self.call(0x7ae140, [case.get('query_flags', 0x100111)])
        functor = self.alloc(struct.pack('<5IHH', overflow, polygons, vertices,
                                         indices_ptr, query_box, (ignore | 0x80) & 0xffff, 0))
        traversal = self.alloc(struct.pack('<2I', bsp, functor))
        write(0xd2dbf8, '2I', 0, 0)
        write(0xcdd7a0, 'I', self.cache if case.get('cache', True) else 0)
        cpu.mem_write(self.cache, bytes(0x1000))  # Invalidate node keys.
        write(0xcdf7bc, 'I', 0)
        self.call(0x7ca920, [0, query_box, bounds], traversal)
        tested, hit = struct.unpack('<2I', cpu.mem_read(0xd2dbf8, 8))
        if max(tested, hit) > 8192:
            raise RuntimeError('Invalid original BSP result size')
        return dict(name=case['name'],
                    tested=list(struct.unpack('<'+'H'*tested, cpu.mem_read(0xd25bf8, tested*2))),
                    faces=list(struct.unpack('<'+'H'*hit, cpu.mem_read(0xd29bf8, hit*2))),
                    overflow=struct.unpack('<I', cpu.mem_read(overflow, 4))[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    args = parser.parse_args()
    original = OriginalWmoQuery(args.binary)
    for line in sys.stdin:
        if line.strip():
            print(json.dumps(original.query(json.loads(line))), flush=True)


if __name__ == '__main__':
    main()
