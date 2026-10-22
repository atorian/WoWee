#!/usr/bin/env python3
"""Execute the original entity-list collector and M2 facets with a fixture callback."""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EIP,UC_X86_REG_EAX
from original_collision_reference import OriginalCollision

class OriginalEntityQuery(OriginalCollision):
    def __init__(self,binary):
        super().__init__(binary)
        self.cpu.hook_add(UC_HOOK_CODE,self.callback,begin=0x1000100,end=0x1000100)
    def callback(self,cpu,address,size,user):
        sp=cpu.reg_read(UC_X86_REG_ESP);target,guid,param,data,flags=struct.unpack('<5I',cpu.mem_read(sp,20))
        owner=struct.unpack('<Q',cpu.mem_read(guid,8))[0];self.calls.append(dict(owner=owner,parameter=param,flags=flags))
        p,model=self.models[param]
        if p['available']:
            cpu.mem_write(data,struct.pack('<I6f',model,*p['bounds']))
            cpu.mem_write(data+0x20,struct.pack('<16f',*p['matrix']))
            cpu.mem_write(guid,struct.pack('<Q',p['returned_owner']))
        cpu.reg_write(UC_X86_REG_EAX,int(p['available']));cpu.reg_write(UC_X86_REG_ESP,sp+4);cpu.reg_write(UC_X86_REG_EIP,target)
    def query(self,c):
        self.next=0x2000000;self.calls=[];self.models={};cpu=self.cpu
        def write(at,fmt,*v):cpu.mem_write(at,struct.pack('<'+fmt,*v))
        entities=[]
        for p in c['objects']:
            obj=self.alloc(bytes(0x100));entities.append(obj)
            write(obj+0x7c,'I',p['static']);write(obj+0x2c,'I',p['tag']);write(obj+0x98,'QI',p['owner'],p['parameter'])
            model=self.alloc(bytes(0x100));shared=self.alloc(bytes(0x200));data=self.alloc(bytes(0x200))
            write(model+0x10,'I',1);write(model+0x2c,'I',shared);write(shared+0x150,'I',data)
            points=self.floats(sum(p['vertices'],[]));normals=self.floats(sum(p['normals'],[]));indices=sum(p['triangles'],[])
            indices_data=self.alloc(struct.pack('<'+'H'*len(indices),*indices))
            write(data+0xd8,'6I',len(indices),indices_data,len(p['vertices']),points,len(p['normals']),normals)
            self.models[p['parameter']]=(p,model)
        count=max([len(p['vertices']) for p in c['objects']]+[1])
        positions=self.alloc(bytes(count*12));codes=self.alloc(bytes(count*4))
        write(0xd411a0,'4I',count,count,positions,0);write(0xd411b0,'4I',count,count,codes,0)
        capacity=1000;out=self.alloc(bytes(capacity*52));ids=self.alloc(bytes(capacity*8))
        data=self.alloc(struct.pack('<8I',capacity,0,out,0,capacity,0,ids,0))
        for call in c['calls']:
            head=self.alloc(bytes(12));write(head,'I',8);prev=None
            for i in call['links']:
                link=self.alloc(bytes(16));write(link+4,'I',entities[i]);write(prev+12 if prev else head+8,'I',link);prev=link
            write(0xce04c4,'I',call['tag']);write(0xce04b0,'I',0x1000100 if call['provider'] else 0)
            self.call(0x7a5240,[head,self.floats(call['box']),data,call['flags']])
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>capacity:raise RuntimeError('Entity fixture overflow')
        facets=[]
        for i in range(count):
            v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],owner=struct.unpack('<Q',cpu.mem_read(ids+i*8,8))[0]))
        tags=[struct.unpack('<I',cpu.mem_read(o+0x2c,4))[0] for o in entities]
        return dict(name=c['name'],tags=tags,calls=self.calls,facets=facets)
