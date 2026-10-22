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

    def impassable(self,case):
        self.next=0x2000000
        chunk=self.alloc(bytes(0x100))
        self.cpu.mem_write(chunk+0x4c,struct.pack('<6f',*case['bounds']))
        box=self.floats(case['box'])
        out=self.alloc(bytes(8*52)); data=self.alloc(struct.pack('<8I',8,0,out,0,0,0,0,0))
        self.call(0x7a43d0,[chunk,box,data])
        count=struct.unpack('<I',self.cpu.mem_read(data+4,4))[0]
        if count>8: raise RuntimeError('Invalid original impassable facet count')
        facets=[]
        for i in range(count):
            values=list(struct.unpack('<13f',self.cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=values[:4],vertices=[values[j:j+3] for j in [4,7,10]],owner=0))
        return dict(name=case['name'],facets=facets,diagnostics=0)

    def liquid(self,case):
        self.next=0x2000000
        self.diagnostics=0
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        chunk=self.alloc(bytes(0x100));write(chunk+0x7c,'3f',*case['corner'])
        liquid=self.alloc(bytes(0x500));write(liquid+0x5c,'I',chunk)
        b=case['bounds'];write(liquid+0x34,'4i',b[1],b[0],b[3],b[2])
        write(liquid+4,'I',case.get('type',1))
        bits=self.alloc(struct.pack('<Q',case['mask']));write(liquid+0x48,'I',bits)
        if case.get('tiles'):
            tiles=self.alloc(bytes(case['tiles']));write(liquid+0x54,'I',tiles)
        write(liquid+0x78,'f'*len(sum(case['vertices'],[])),*sum(case['vertices'],[]))
        r=case['rect'];rect=self.alloc(struct.pack('<4i',r[1],r[0],r[3],r[2]))
        box=self.floats(case['box'])
        out=self.alloc(bytes(128*52));ids=self.alloc(bytes(128*8))
        data=self.alloc(struct.pack('<8I',128,0,out,0,128,0,ids,0))
        write(0xcf08f8,'I',int(case.get('sse',False)))
        self.call(0x7a3cf0,[chunk,box,rect,liquid,data])
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>128:raise RuntimeError('Invalid original liquid count')
        facets=[]
        for i in range(count):
            values=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=values[:4],vertices=[values[j:j+3] for j in [4,7,10]],owner=0))
        return dict(name=case['name'],facets=facets,diagnostics=self.diagnostics)

    def flight(self,case):
        self.next=0x2000000
        self.diagnostics=0
        cpu=self.cpu
        area=self.alloc(bytes(0x100))
        heights=self.alloc(struct.pack('<18h',*case['flight_heights']))
        cpu.mem_write(area+0xb0,struct.pack('<I',heights))
        cpu.mem_write(area+0x3c,struct.pack('<3f',*case['corner']))
        cpu.mem_write(0xce48d0,struct.pack('<I',area))
        box=self.floats(case['box'])
        out=self.alloc(bytes(16*52));ids=self.alloc(bytes(16*8))
        data=self.alloc(struct.pack('<8I',16,0,out,0,16,0,ids,0))
        self.call(0x7a4590,[0,0,box,data])
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>16: raise RuntimeError('Invalid original flight facet count')
        facets=[]
        for i in range(count):
            values=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=values[:4],vertices=[values[j:j+3] for j in [4,7,10]],owner=0))
        return dict(name=case['name'],facets=facets,diagnostics=self.diagnostics)
