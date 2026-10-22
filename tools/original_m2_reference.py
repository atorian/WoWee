#!/usr/bin/env python3
"""Execute original 12340 CM2Model::GetCollisionFacets with resident model data."""
import struct
from original_collision_reference import OriginalCollision


class OriginalM2Query(OriginalCollision):
    def query(self, case):
        self.next=0x2000000
        def write(address, fmt, *values): self.cpu.mem_write(address, struct.pack('<'+fmt,*values))
        vertices=case['vertices'];normals=case['normals'];indices=sum(case['triangles'],[])
        model=self.alloc(bytes(0x100));shared=self.alloc(bytes(0x200));data=self.alloc(bytes(0x200))
        write(model+0x10,'I',1);write(model+0x2c,'I',shared);write(shared+0x150,'I',data)
        points=self.floats(sum(vertices,[]));normal_data=self.floats(sum(normals,[]))
        index_data=self.alloc(struct.pack('<'+'H'*len(indices),*indices))
        write(data+0xd8,'6I',len(indices),index_data,len(vertices),points,len(normals),normal_data)
        positions=self.alloc(bytes(max(1,len(vertices))*12));codes=self.alloc(bytes(max(1,len(vertices))*4))
        write(0xd411a0,'4I',len(vertices),len(vertices),positions,0)
        write(0xd411b0,'4I',len(vertices),len(vertices),codes,0)
        output=self.alloc(bytes(max(1,len(normals))*52));array=self.alloc(struct.pack('<4I',len(normals),0,output,0))
        box=self.floats(case['box']);matrix=self.floats(case['matrix'])
        self.call(0x82ec30,[box,matrix,array],model)
        count=struct.unpack('<I',self.cpu.mem_read(array+4,4))[0]
        result=[]
        for i in range(count):
            v=list(struct.unpack('<13f',self.cpu.mem_read(output+i*52,52)))
            result.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],owner=case.get('owner',0)))
        return dict(name=case['name'],facets=result)
