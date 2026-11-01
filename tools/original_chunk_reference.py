#!/usr/bin/env python3
"""Execute original loaded chunk collection with original bounds/liquid setup."""
import struct
from original_terrain_reference import OriginalTerrainQuery

class OriginalChunkQuery(OriginalTerrainQuery):
    def build_chunk(self,case,area=None):
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        x,y=case['chunk'];area=area or self.alloc(bytes(0x500));chunk=self.alloc(bytes(0x200))
        areaIndex=(x>>4)+((y>>4)<<6)
        write(0xce48d0+areaIndex*4,'I',area)
        write(area+0xbc+((y&15)*16+(x&15))*4,'I',chunk)
        header=self.alloc(bytes(128));write(header+0x3c,'H',case['holes'])
        heights=self.floats(case['heights'])
        write(chunk+0x110,'I',header);write(chunk+0x11c,'I',heights)
        write(chunk+0x34,'2i',x,y);write(chunk+0x84,'f',case['base_z'])
        self.call(0x7c5220,[],chunk)
        # CreateBounds computes the same top-left XY as Create; all coordinate
        # and min/max-height arithmetic here runs in the original executable.
        hi=struct.unpack('<3f',cpu.mem_read(chunk+0x58,12))
        write(chunk+0x7c,'3f',hi[0],hi[1],case['base_z'])
        write(chunk+0xc,'I',0x40 if case['impassable'] else 0)
        nodes=[];records=self.alloc(bytes(32*4))
        write(0xad4074,'I',0);write(0xad4070,'I',31);write(0xad4084,'I',records)
        for layer in case['liquids']:
            liquid=self.alloc(bytes(0x500));nodes.append(liquid)
            type=15 if layer['legacy'] and case['map_id']==530 and layer['type']==3 else layer['type']
            record=self.alloc(bytes(16));write(record+8,'I',layer['flags']);write(records+type*4,'I',record)
            write(liquid+4,'I',type);write(liquid+0x5c,'I',chunk)
            b=layer['bounds'];write(liquid+0x34,'4i',b[1],b[0],b[3],b[2])
            reader=self.alloc(bytes(32));write(liquid+0x44,'I',reader)
            vtable=self.alloc(struct.pack('<2I',0,0x8a3200 if layer['legacy'] else 0x8a3130));write(reader,'I',vtable)
            if layer['legacy']:
                data=self.alloc(b''.join(struct.pack('<If',0,h) for h in layer['heights']))
                write(reader+8,'2I',data,layer['format'])
                write(liquid+0x54,'I',self.alloc(bytes(layer['tiles'])))
            else:
                data=self.floats(layer['heights']);desc=self.alloc(bytes(24))
                write(desc+2,'H',layer['format']);write(reader+8,'2I',data,desc)
            write(liquid+0x48,'I',self.alloc(struct.pack('<Q',layer['mask'])))
            self.call(0x7cdf80,[],liquid)
        write(chunk+0x100,'I',0x70);write(chunk+0x108,'I',nodes[0] if nodes else 0)
        for i,node in enumerate(nodes):write(node+0x74,'I',nodes[i+1] if i+1<len(nodes) else 0)
        return area,chunk

    def query(self,case):
        self.next=0x2000000;self.diagnostics=0
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        x,y=case['chunk'];area,chunk=self.build_chunk(case)
        r=case['rect'];rect=self.alloc(struct.pack('<4i',r[1],r[0],r[3],r[2]))
        box=self.floats(case['box']);body=self.floats(case['box'])
        out=self.alloc(bytes(1024*52));ids=self.alloc(bytes(1024*8))
        data=self.alloc(struct.pack('<8I',1024,0,out,0,1024,0,ids,0))
        write(0xcf08f8,'I',int(case.get('sse',False)))
        result=self.call(0x7a5a60,[x,y,rect,body,box,data,case['flags']])
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>1024:raise RuntimeError('Invalid original chunk facet count')
        facets=[]
        for i in range(count):
            v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],owner=0))
        return dict(name=case['name'],ready=bool(result&255),facets=facets)

    def unavailable(self,case):
        self.next=0x2000000
        cpu=self.cpu
        def write(address,fmt,*values):cpu.mem_write(address,struct.pack('<'+fmt,*values))
        x,y=case['chunk'];index=((x>>4)&63)+(((y>>4)&63)<<6)
        # Both absent and asynchronous areas must take this original branch.
        area=self.alloc(bytes(0x500)) if case['loading'] else 0
        if area:write(area+0x70,'I',1)
        write(0xce48d0+index*4,'I',area)
        write(0xce88d0+index*8,'I',int(case['terrain']))
        body=self.floats(case['body']);box=self.floats(case['box'])
        rect=self.alloc(bytes(16));out=self.alloc(bytes(12*52));ids=self.alloc(bytes(12*8))
        data=self.alloc(struct.pack('<8I',12,0,out,0,12,0,ids,0))
        result=self.call(0x7a5a60,[x,y,rect,body,box,data,case['flags']])
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>12:raise RuntimeError('Invalid original unavailable facet count')
        facets=[]
        for i in range(count):
            v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],
                owner=struct.unpack('<Q',cpu.mem_read(ids+i*8,8))[0]))
        return dict(name=case['name'],ready=bool(result&255),facets=facets)
