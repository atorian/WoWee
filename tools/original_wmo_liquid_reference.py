#!/usr/bin/env python3
"""Run original WMO GenLiquidVerts/GetLiquidFacets, without renderer geometry."""
import struct
from original_facet_reference import OriginalFacetBuilder

class OriginalWmoLiquid(OriginalFacetBuilder):
    def query(self,case):
        self.next=0x2000000;self.diagnostics=0
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        group=self.alloc(bytes(0x300));w,h=case['size'];count=(w+1)*(h+1)
        vertices=self.alloc(bytes(count*12));array=self.alloc(bytes(24))
        write(array+8,'3I',count,count,vertices);write(group+0x1c,'I',array)
        raw=self.alloc(b''.join(struct.pack('<If',0,height) for height in case['heights']))
        write(group+0x134,'I',raw);write(group+0x114,'4i',w+1,h+1,w,h)
        write(group+0x124,'3f',*case['corner']);write(group+0x144,'I',case['type'])
        write(group+0x138,'I',self.alloc(bytes(case['tiles'])))
        self.call(0x7c8c60,[],group)
        record=self.alloc(bytes(16));write(record+8,'I',case['liquid_flags'])
        rows=self.alloc(struct.pack('<I',record))
        write(0xad4074,'I',case['type']);write(0xad4070,'I',case['type']);write(0xad4084,'I',rows)
        placement=self.alloc(bytes(0x200));write(placement+0x70,'16f',*case['matrix'])
        write(placement+0x148,'Q',case['owner'])
        box=self.floats(case['box']);capacity=max(1,w*h*2)
        out=self.alloc(bytes(capacity*52));ids=self.alloc(bytes(capacity*8))
        data=self.alloc(struct.pack('<8I',capacity,0,out,0,capacity,0,ids,0))
        write(0xcf08f8,'I',int(case.get('sse',False)))
        self.call(0x7ca110,[box,case['flags'],data,placement],group)
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>capacity:raise RuntimeError('Invalid original WMO liquid count')
        facets=[]
        for i in range(count):
            v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            owner=struct.unpack('<Q',cpu.mem_read(ids+i*8,8))[0]
            facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],owner=owner))
        return dict(name=case['name'],facets=facets,diagnostics=self.diagnostics)
