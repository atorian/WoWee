#!/usr/bin/env python3
"""Execute GetDoodadDefFacets and real M2/barrier generation, with shared links."""
import struct
from original_facet_reference import OriginalFacetBuilder

class OriginalDoodadQuery(OriginalFacetBuilder):
    def build_doodad(self,p):
        def write(at,fmt,*v):self.cpu.mem_write(at,struct.pack('<'+fmt,*v))
        obj=self.alloc(bytes(0x200))
        write(obj+12,'I',p['flags']);write(obj+0x7c,'I',p['static']);write(obj+0x2c,'I',p['tag'])
        write(obj+0xb8,'Q',p['owner']);write(obj+0xc0,'6f',*p['bounds']);write(obj+0xd8,'16f',*p['matrix'])
        if not p['present']:return obj
        model=self.alloc(bytes(0x100));shared=self.alloc(bytes(0x200));data=self.alloc(bytes(0x200))
        write(obj+0x34,'I',model);write(model+0x10,'I',1);write(model+0x2c,'I',shared)
        write(shared+0x150,'I',data);write(shared+0x154,'6f',*p['model_bounds'])
        indices=sum(p['triangles'],[])
        points=self.floats(sum(p['vertices'],[]));normals=self.floats(sum(p['normals'],[]))
        index_data=self.alloc(struct.pack('<'+'H'*len(indices),*indices))
        write(data+0xd8,'6I',len(indices),index_data,len(p['vertices']),points,len(p['normals']),normals)
        return obj

    def query(self,c):
        self.next=0x2000000;self.diagnostics=0;cpu=self.cpu
        def write(at,fmt,*v):cpu.mem_write(at,struct.pack('<'+fmt,*v))
        objects=[]
        for p in c['objects']:
            objects.append(self.build_doodad(p))
        count=max([len(p['vertices']) for p in c['objects']]+[1])
        positions=self.alloc(bytes(count*12));codes=self.alloc(bytes(count*4))
        write(0xd411a0,'4I',count,count,positions,0);write(0xd411b0,'4I',count,count,codes,0)
        capacity=4000;out=self.alloc(bytes(capacity*52));ids=self.alloc(bytes(capacity*8))
        data=self.alloc(struct.pack('<8I',capacity,0,out,0,capacity,0,ids,0));write(0xcf08f8,'I',0)
        for call in c['calls']:
            head=self.alloc(bytes(12));write(head,'I',8);prev=None
            for index in call['links']:
                link=self.alloc(bytes(16));write(link+4,'I',objects[index])
                write(prev+12 if prev else head+8,'I',link);prev=link
            write(0xce04c4,'I',call['tag'])
            self.call(0x7a50c0,[head,self.floats(call['box']),data,call['flags']])
        n=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if n>capacity:raise RuntimeError('Doodad fixture overflow')
        facets=[]
        for i in range(n):
            v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],owner=struct.unpack('<Q',cpu.mem_read(ids+i*8,8))[0]))
        state=[dict(tag=struct.unpack('<I',cpu.mem_read(o+0x2c,4))[0],static=struct.unpack('<I',cpu.mem_read(o+0x7c,4))[0]) for o in objects]
        return dict(name=c['name'],state=state,facets=facets,diagnostics=self.diagnostics)
