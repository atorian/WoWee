#!/usr/bin/env python3
"""Execute build 12340's terrain grid initialization and chunk facet query."""
import struct
from original_facet_reference import OriginalFacetBuilder
from unicorn import UC_HOOK_CODE


class OriginalTerrainQuery(OriginalFacetBuilder):
    def __init__(self,binary,cpu_reference=None):
        super().__init__(binary,cpu_reference)
        self.cpu.hook_add(UC_HOOK_CODE,self._rsqrt,begin=0x7d8d2b,end=0x7d8d2b)
        self.call(0x7c3c60,[])

    def query(self,case):
        self.next=0x2000000
        self.diagnostics=0
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        header=self.alloc(bytes(0x80));write(header+0x3c,'H',case.get('holes',0))
        heights=self.floats(case['heights'])
        chunk=self.alloc(bytes(0x200))
        write(chunk+0x110,'I',header);write(chunk+0x11c,'I',heights)
        write(chunk+0x7c,'3f',*case.get('corner',[0,0,0]))
        # CiRect keeps Y before X; public fixtures use [minX,minY,maxX,maxY].
        r=case.get('rect',[0,0,7,7]);rect=self.alloc(struct.pack('<4i',r[1],r[0],r[3],r[2]))
        box=self.floats(case['box'])
        out=self.alloc(bytes(256*52));data=self.alloc(struct.pack('<4I',256,0,out,0))
        write(0xcf08f8,'I',int(case.get('sse',False)));write(0xcd774c,'I',0)
        self.call(0x7d8840,[rect,box,data],chunk)
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>256:raise RuntimeError('Invalid original terrain count')
        facets=[]
        for i in range(count):
            values=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=values[:4],vertices=[values[j:j+3] for j in [4,7,10]],owner=0))
        return dict(name=case['name'],facets=facets,diagnostics=self.diagnostics)
