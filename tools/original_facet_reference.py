#!/usr/bin/env python3
"""Execute build 12340 TriDataToFacetData, including its selected CPU path."""
import ctypes
from pathlib import Path
import struct
from original_collision_reference import OriginalCollision
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_XMM0, UC_X86_REG_ESP, UC_X86_REG_EIP


class OriginalFacetBuilder(OriginalCollision):
    def __init__(self, binary, cpu_reference=None):
        super().__init__(binary)
        self.cpu_reference = None
        if cpu_reference:
            self.cpu_reference = ctypes.CDLL(str(Path(cpu_reference).resolve()))
            self.cpu_reference.original_rsqrtss.argtypes = [ctypes.c_uint32]
            self.cpu_reference.original_rsqrtss.restype = ctypes.c_uint32
        self.cpu.hook_add(UC_HOOK_CODE, self._rsqrt, begin=0x7829ff, end=0x7829ff)
        self.cpu.hook_add(UC_HOOK_CODE, self._diagnostic, begin=0x4b5040, end=0x4b5040)

    def _rsqrt(self, cpu, address, size, user_data):
        # Unicorn implements RSQRTSS as exact reciprocal square root. Real x86
        # deliberately returns an approximation, which changes collision planes.
        # Execute only this instruction on the host CPU; all original geometry
        # arithmetic, branching and stores continue in the original machine code.
        if self.cpu_reference is None:
            raise RuntimeError('SSE reference requires --cpu-reference built from original_cpu_reference.cpp')
        value = cpu.reg_read(UC_X86_REG_XMM0)
        result = self.cpu_reference.original_rsqrtss(value & 0xffffffff)
        cpu.reg_write(UC_X86_REG_XMM0, (value & ~0xffffffff) | result)
        cpu.reg_write(UC_X86_REG_EIP, address + size)

    def _diagnostic(self, cpu, address, size, user_data):
        # SysMsgPrintf would need the OS/UI. Record its observable diagnostic and
        # return from this varargs output-only call; never replace a math routine.
        self.diagnostics += 1
        stack = cpu.reg_read(UC_X86_REG_ESP)
        target = struct.unpack('<I', cpu.mem_read(stack, 4))[0]
        cpu.reg_write(UC_X86_REG_ESP, stack + 4)
        cpu.reg_write(UC_X86_REG_EIP, target)

    def build(self, case):
        self.next = 0x2000000
        self.diagnostics = 0
        cpu = self.cpu
        def write(address, fmt, *values):
            cpu.mem_write(address, struct.pack('<'+fmt, *values))
        batches = case['batches']
        count = sum(len(b['triangles']) for b in batches)
        output = self.alloc(bytes(max(1,count)*52))
        owners = self.alloc(bytes(max(1,count)*8))
        data = self.alloc(struct.pack('<8I', count,0,output,0,count,0,owners,0))
        if len(batches)>32:raise ValueError('Too many reference batches')
        for index,b in enumerate(batches):
            base = 0xcd8080 + index*0x24
            matrix = self.floats(b['matrix'])
            vertices = self.floats(sum(b['vertices'],[]))
            indices = sum(b['triangles'],[])
            indices = self.alloc(struct.pack('<'+'H'*len(indices), *indices))
            write(base,'2I',matrix,vertices)
            write(base+0x10,'I',indices)
            write(base+0x1a,'H',len(b['triangles']))
        write(0xcb752c,'I',len(batches))
        write(0xcf08f8,'I',int(case.get('sse',False)))
        owner = case.get('owner',0)
        self.call(0x782740,[0,data,owner&0xffffffff,owner>>32])
        facets=[]
        for i in range(count):
            values=list(struct.unpack('<13f',cpu.mem_read(output+i*52,52)))
            facets.append(dict(plane=values[:4],vertices=[values[j:j+3] for j in [4,7,10]],
                               owner=struct.unpack('<Q',cpu.mem_read(owners+i*8,8))[0]))
        return dict(name=case['name'],facets=facets,diagnostics=self.diagnostics)
