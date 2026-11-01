#!/usr/bin/env python3
"""Execute GetFacets with real WMO, terrain, M2 and entity-list collectors."""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EIP,UC_X86_REG_EAX
from original_chunk_reference import OriginalChunkQuery
from original_wmo_model_reference import OriginalWmoModel
from original_doodad_reference import OriginalDoodadQuery

class OriginalWorldQuery(OriginalChunkQuery):
    def __init__(self,binary):
        super().__init__(binary)
        self.cache=0x4000000;self.cpu.mem_map(self.cache,0x1000000)
        self.cpu.hook_add(UC_HOOK_CODE,self.entity,begin=0x1000100,end=0x1000100)
    def entity(self,cpu,address,size,user):
        sp=cpu.reg_read(UC_X86_REG_ESP);target,guid,param,data,flags=struct.unpack('<5I',cpu.mem_read(sp,20))
        self.callback_count+=1
        if self.available:
            cpu.mem_write(data,struct.pack('<I6f',self.entity_model,-10,-40,-5,0,-30,5))
            cpu.mem_write(data+0x20,struct.pack('<16f',*self.identity))
        cpu.reg_write(UC_X86_REG_EAX,int(self.available));cpu.reg_write(UC_X86_REG_ESP,sp+4);cpu.reg_write(UC_X86_REG_EIP,target)
    def setup_world(self,c):
        self.diagnostics=0;self.callback_count=0;self.available=c['available'];cpu=self.cpu
        def write(at,fmt,*v):cpu.mem_write(at,struct.pack('<'+fmt,*v))
        self.identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
        cpu.mem_write(0xce48d0,bytes(4096*4));cpu.mem_write(0xce88d0,bytes(4096*8))
        for at in [0xaeede8,0xaeede0,0xd25440,0xd25438]:write(at,'I',0)
        write(0xcf08f4,'I',int(c['dungeon']));write(0xce04c4,'I',c['tag']);write(0xcf08f8,'I',0)
        write(0xce04b0,'I',0x1000100 if c['provider'] else 0)
        def doodad(z,owner,flags=0x80):
            return dict(flags=flags,static=0,tag=0,owner=owner,present=True,bounds=[-10,-40,-5,0,-30,5],
                model_bounds=[-10,-40,-5,0,-30,5],matrix=self.identity,
                vertices=[[-10,-40,z],[0,-40,z],[-5,-30,z]],triangles=[[0,1,2]],normals=[[0,0,1]])
        d=OriginalDoodadQuery.build_doodad(self,doodad(2,c['owner'],c['doodad_flags']))
        ed=OriginalDoodadQuery.build_doodad(self,doodad(4,0))
        self.entity_model=struct.unpack('<I',cpu.mem_read(ed+0x34,4))[0]
        entity=self.alloc(bytes(0x100));write(entity+0x7c,'I',c['entity_flags']);write(entity+0x98,'QI',0xf110000012345678,7)
        def link(head,obj):
            node=self.alloc(bytes(16));write(node+4,'I',obj);write(head,'I',8);write(head+8,'I',node)
        if c['wmo']:
            g=dict(ready=c['wmo']!=4,flags=0,mogi=[-10,-40,-5,0,-30,5],mogp=[-10,-40,-5,0,-30,5],
                vertices=[[-10,-40,3],[0,-40,3],[-5,-30,3]],triangles=[[0,1,2]],mopy=[8],faces=[0],
                liquid=dict(size=[1,1],corner=[-5,-35,0],type=1,flags=4,heights=[1]*4,tiles=[0]))
            root=OriginalWmoModel.build_model(self,[g]);write(root+0x1a8,'6f',-10,-40,-5,0,-30,5)
            write(root+0x1e0,'I',int(c['wmo']!=3))
            placement=self.alloc(bytes(0x200));write(0xd25440,'I',placement);write(placement+0xc,'I',0x100 if c['wmo']==2 else 0)
            write(placement+0x48,'6f',-10,-40,-5,0,-30,5);write(placement+0x70,'16f',*self.identity);write(placement+0xb0,'16f',*self.identity)
            write(placement+0xf4,'I',root);write(placement+0x148,'Q',100)
            if c['wmo']!=5:
                group=self.alloc(bytes(0x100));write(group+0x24,'6f',-10,-40,-5,0,-30,5)
                link(placement+0x114,group);link(group+0x78,d);link(group+0x84,entity)
        area=None
        if c['area']>=3:
            for i in range(2):
                if c['area']==4+i:continue
                case=dict(chunk=[512+i,512],holes=c['holes'],heights=[float(i)]*145,base_z=0,impassable=c['impassable'],liquids=[],map_id=0)
                area,chunk=self.build_chunk(case,area)
                link(chunk+0xc4,d);link(chunk+0xdc,entity)
            write(area+0x3c,'3f',0,0,0)
            if c['flight']:write(area+0xb0,'I',self.alloc(struct.pack('<18h',*([4]*9+[-4]*9))))
        elif c['area']==2:
            area=self.alloc(bytes(0x500));write(area+0x70,'I',1);write(0xce48d0+(32+32*64)*4,'I',area)
        write(0xce88d0+(32+32*64)*8,'I',int(c['area']!=0))
        # Chunk setup installs its material table; the model's type remains row 1.
        rows=self.alloc(bytes(32*4));record=self.alloc(bytes(16));write(record+8,'I',4);write(rows+4,'I',record)
        write(0xad4074,'I',0);write(0xad4070,'I',31);write(0xad4084,'I',rows)
        positions=self.alloc(bytes(36));codes=self.alloc(bytes(12));write(0xd411a0,'4I',3,3,positions,0);write(0xd411b0,'4I',3,3,codes,0)
        cpu.mem_write(self.cache,bytes(0x1000));write(0xcdd7a0,'I',self.cache);write(0xcdf7bc,'I',0);write(0xd2dbf8,'2I',0,0)
        self.doodad=d;self.world_entity=entity

    def query(self,c):
        self.next=0x2000000;self.setup_world(c);cpu=self.cpu
        d=self.doodad;entity=self.world_entity
        capacity=2048;out=self.alloc(bytes(capacity*52));ids=self.alloc(bytes(capacity*8));data=self.alloc(struct.pack('<8I',capacity,0,out,0,capacity,0,ids,0))
        results=[]
        for flags in c['flags']:
            status=self.alloc(bytes(4));ready=bool(self.call(0x7a5f20,[self.floats(c['body']),self.floats(c['box']),data,flags,status])&255)
            count=struct.unpack('<I',cpu.mem_read(data+4,4))[0]
            if count>capacity:raise RuntimeError('World fixture overflow')
            facets=[]
            for i in range(count):
                v=list(struct.unpack('<13f',cpu.mem_read(out+i*52,52)))
                facets.append(dict(plane=v[:4],vertices=[v[j:j+3] for j in [4,7,10]],owner=struct.unpack('<Q',cpu.mem_read(ids+i*8,8))[0]))
            results.append(dict(ready=ready,status=struct.unpack('<I',cpu.mem_read(status,4))[0],facets=facets,
                tag=struct.unpack('<I',cpu.mem_read(0xce04c4,4))[0],doodad_tag=struct.unpack('<I',cpu.mem_read(d+0x2c,4))[0],
                entity_tag=struct.unpack('<I',cpu.mem_read(entity+0x2c,4))[0],callbacks=self.callback_count))
        return dict(name=c['name'],results=results)
