#!/usr/bin/env python3
"""Execute GetMapObjFacets, including transforms, model geometry and barriers.

Only linked doodad/entity service boundaries are supplied by the fixture.
Their calls are recorded; this does not establish their internal equivalence.
"""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX, UC_X86_REG_EIP
from original_wmo_model_reference import OriginalWmoModel

class OriginalWmoWorld(OriginalWmoModel):
    def __init__(self,binary,cpu_reference=None):
        super().__init__(binary,cpu_reference)
        for address in [0x7a50c0,0x7a5240]:
            self.cpu.hook_add(UC_HOOK_CODE,self.linked,begin=address,end=address)

    def linked(self,cpu,address,size,user):
        sp=cpu.reg_read(UC_X86_REG_ESP)
        target,link,box,data,flags=struct.unpack('<5I',cpu.mem_read(sp,20))
        identity,ready=self.links[link]
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        self.calls.append(dict(kind=int(address==0x7a5240),link=identity,flags=flags,
            count=count,box=list(struct.unpack('<6f',cpu.mem_read(box,24)))))
        cpu.reg_write(UC_X86_REG_EAX,int(ready))
        cpu.reg_write(UC_X86_REG_ESP,sp+4);cpu.reg_write(UC_X86_REG_EIP,target)

    def query(self,c):
        self.next=0x2000000;self.diagnostics=0;self.calls=[];self.links={}
        cpu=self.cpu
        def write(at,fmt,*v):cpu.mem_write(at,struct.pack('<'+fmt,*v))
        previous=None;write(0xaeede8,'I',0);write(0xaeede0,'I',0)
        for proxy in c['proxies']:
            node=self.alloc(bytes(32));write(node+12,'4f',*proxy)
            write(previous+4 if previous else 0xaeede8,'I',node);previous=node
        previous=None;write(0xd25440,'I',0);write(0xd25438,'I',0)
        for p in c['placements']:
            root=self.build_model(p['groups'])
            write(root+0x1e0,'I',int(p['ready']));write(root+0x1a8,'6f',*p['root'])
            node=self.alloc(bytes(0x200));write(previous+4 if previous else 0xd25440,'I',node);previous=node
            write(node+12,'I',p['flags']);write(node+0x48,'6f',*p['bounds'])
            write(node+0x70,'16f',*p['matrix']);write(node+0xb0,'16f',*p['inverse'])
            write(node+0xf4,'I',root if p['present'] else 0);write(node+0x148,'Q',p['owner'])
            write(node+0x114,'I',8)
            prevlink=None
            for g in p['links']:
                link=self.alloc(bytes(16));group=self.alloc(bytes(0x100));write(link+4,'I',group)
                write(prevlink+12 if prevlink else node+0x11c,'I',link);prevlink=link
                write(group+0x24,'6f',*g['bounds']);write(group+0x50,'I',g['index'])
                self.links[group+0x78]=(g['id'],g['ready']);self.links[group+0x84]=(g['id'],True)
        cpu.mem_write(self.cache,bytes(0x1000));write(0xcdd7a0,'I',self.cache);write(0xcdf7bc,'I',0)
        write(0xd2dbf8,'2I',0,0);write(0xcb7528,'5I',0,0,0,0,0);write(0xcf08f8,'I',int(c['sse']))
        capacity=2000
        out=self.alloc(bytes(capacity*52));ids=self.alloc(bytes(capacity*8))
        data=self.alloc(struct.pack('<8I',capacity,0,out,0,capacity,0,ids,0))
        status=self.alloc(struct.pack('<I',c['status']))
        ready=bool(self.call(0x7a55e0,[self.floats(c['body']),self.floats(c['box']),data,c['flags'],status])&255)
        count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
        if count>capacity:raise RuntimeError('World fixture capacity exceeded')
        facets=[]
        for i in range(count):
            v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
            facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],owner=struct.unpack('<Q',cpu.mem_read(ids+i*8,8))[0]))
        return dict(name=c['name'],ready=ready,status=struct.unpack('<I',cpu.mem_read(status,4))[0],diagnostics=self.diagnostics,calls=self.calls,facets=facets)
