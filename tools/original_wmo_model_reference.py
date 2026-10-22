#!/usr/bin/env python3
"""Execute original model/group selection, BSP, batch pools and facet conversion."""
import struct
from original_facet_reference import OriginalFacetBuilder

class OriginalWmoModel(OriginalFacetBuilder):
    def __init__(self,binary,cpu_reference=None):
        super().__init__(binary,cpu_reference)
        self.cache=0x4000000
        self.cpu.mem_map(self.cache,0x1000000)

    def build_model(self,groups):
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        root=self.alloc(bytes(0x1f8+len(groups)*4))
        infos=self.alloc(b''.join(struct.pack('<I6fi',0,*g['mogi'],0) for g in groups))
        write(root+0x130,'I',infos);write(root+0x16c,'I',len(groups));write(root+0x1e0,'I',1)
        rows=self.alloc(bytes(32*4));write(0xad4074,'I',0);write(0xad4070,'I',31);write(0xad4084,'I',rows)
        for i,g in enumerate(groups):
            group=self.alloc(bytes(0x300));write(root+0x1f8+i*4,'I',group)
            write(group+0x198,'I',int(g['ready']));write(group+0x30,'I',g['flags'])
            vertices=self.floats(sum(g['vertices'],[]));indices=sum(g['triangles'],[])
            indices=self.alloc(struct.pack('<'+'H'*len(indices),*indices))
            polys=self.alloc(b''.join(bytes([f,0]) for f in g['mopy']))
            nodes=self.alloc(struct.pack('<4HIf',4,0xffff,0xffff,len(g['faces']),0,0))
            faces=self.alloc(struct.pack('<'+'H'*len(g['faces']),*g['faces']))
            write(group+0x64,'5I',nodes,nodes,faces,1,len(g['faces']))
            write(group+0xb0,'6f',*g['mogp']);write(group+0xdc,'4I',polys,indices,0,vertices)
            liquid=g.get('liquid')
            if liquid:
                w,h=liquid['size'];count=(w+1)*(h+1)
                verts=self.alloc(bytes(count*12));array=self.alloc(bytes(24))
                write(array+8,'3I',count,count,verts);write(group+0x1c,'I',array)
                raw=self.alloc(b''.join(struct.pack('<If',0,z) for z in liquid['heights']))
                write(group+0x134,'I',raw);write(group+0x114,'4i',w+1,h+1,w,h)
                write(group+0x124,'3f',*liquid['corner']);write(group+0x144,'I',liquid['type'])
                write(group+0x138,'I',self.alloc(bytes(liquid['tiles'])))
                self.call(0x7c8c60,[],group)
                record=self.alloc(bytes(16));write(record+8,'I',liquid['flags']);write(rows+liquid['type']*4,'I',record)
        return root

    def query(self,case):
        self.next=0x2000000;self.diagnostics=0
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        groups=case['groups'];root=self.build_model(groups)
        cpu.mem_write(self.cache,bytes(0x1000));write(0xcdd7a0,'I',self.cache);write(0xcdf7bc,'I',0)
        write(0xd2dbf8,'2I',0,0);write(0xcb7528,'5I',0,0,0,0,0);write(0xcf08f8,'I',int(case['sse']))
        placement=self.alloc(bytes(0x200));write(placement+0x70,'16f',*case['matrix']);write(placement+0x148,'Q',case['owner'])
        box=self.floats(case['box']);tri=self.alloc(bytes(4))
        capacity=2+sum(len(g['faces'])+4*len(g['liquid']['tiles']) if g.get('liquid') else len(g['faces']) for g in groups)
        out=self.alloc(bytes(capacity*52));ids=self.alloc(bytes(capacity*8))
        data=self.alloc(struct.pack('<8I',capacity,0,out,0,capacity,0,ids,0))
        status=0
        if case['flags']&0xf0:
            self.call(0x7aef00,[box,case['flags'],tri,placement],root)
            self.call(0x782740,[tri,data,case['owner']&0xffffffff,case['owner']>>32])
            status=struct.unpack('<I',cpu.mem_read(0xcb7528,4))[0]
        if case['flags']&0x30000:self.call(0x7af000,[box,case['flags'],data,placement],root)
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>capacity:raise RuntimeError('Invalid original WMO model facet count')
        facets=[]
        for i in range(count):
            v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],
                owner=struct.unpack('<Q',cpu.mem_read(ids+i*8,8))[0]))
        return dict(name=case['name'],status=status,diagnostics=self.diagnostics,facets=facets)
