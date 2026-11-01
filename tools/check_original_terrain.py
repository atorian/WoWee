#!/usr/bin/env python3
"""Compare terrain facet selection, holes, order and planes with build 12340."""
import argparse
import json
from pathlib import Path
import random
import subprocess
from check_original_collision import vector,f32
from check_original_facets import same


def cases(sse):
    def case(name,**kw):
        return dict(name=name,sse=sse,heights=kw.pop('heights',[0.0]*145),
                    box=kw.pop('box',[-40,-40,-10,1,1,10]),**kw)
    yield case('whole_chunk')
    for i in range(16):yield case(f'hole_{i}',holes=1<<i)
    yield case('all_holes',holes=65535)
    for axis in range(3):
        for side in [0,1]:
            for edge in [-.01944445,-.019444443,-.01944444,0,.01944444,.019444443,.01944445]:
                box=[-40,-40,-10,1,1,10];box[axis+3*side]=f32(edge)
                yield case(f'boundary_{axis}_{side}_{edge}',box=box,rect=[0,0,0,0])
    rng=random.Random(12340)
    for i in range(400):
        low=[rng.randrange(8),rng.randrange(8)]
        high=[rng.randrange(low[j],min(low[j]+3,7)+1) for j in range(2)]
        corner=vector([rng.uniform(-16000,16000),rng.uniform(-16000,16000),rng.uniform(-1000,1000)])
        heights=vector([rng.uniform(-30,30) for _ in range(145)])
        box=[rng.uniform(-40,0),rng.uniform(-40,0),rng.uniform(-35,10),
             rng.uniform(0,1),rng.uniform(0,1),rng.uniform(11,40)]
        yield case(f'chunk_{i}',rect=low+high,corner=corner,heights=heights,holes=rng.randrange(65536),box=vector(box))


def impassable_cases():
    yield dict(name='equal_bounds',bounds=[0,0,0,1,1,1],box=[0,0,-1,1,1,2])
    for mask in range(16):
        yield dict(name=f'sides_{mask}',bounds=[0,0,0,1,1,1],
                   box=[-1 if mask&1 else 0,-1 if mask&2 else 0,-100,
                        2 if mask&4 else 1,2 if mask&8 else 1,100])
    rng=random.Random(12340)
    for i in range(1000):
        low=vector([rng.uniform(-17000,17000),rng.uniform(-17000,17000),rng.uniform(-2000,2000)])
        high=vector([low[0]+rng.uniform(.001,40),low[1]+rng.uniform(.001,40),low[2]+100])
        box=vector([low[0]+rng.uniform(-10,10),low[1]+rng.uniform(-10,10),low[2]+rng.uniform(-100,100),
                    high[0]+rng.uniform(-10,10),high[1]+rng.uniform(-10,10),high[2]+rng.uniform(100,200)])
        yield dict(name=f'impassable_{i}',bounds=low+high,box=box)


def liquid_cases(sse):
    rng=random.Random(12340)
    for i in range(1000):
        x,y=rng.randrange(8),rng.randrange(8)
        w,h=rng.randrange(1,9-x),rng.randrange(1,9-y)
        vertices=[vector([-row*4.1666665,-col*4.1666665,rng.uniform(-10,10)])
                  for row in range(y,y+h+1) for col in range(x,x+w+1)]
        lo=[rng.randrange(8),rng.randrange(8)]
        hi=[rng.randrange(lo[j],8) for j in range(2)]
        yield dict(name=f'liquid_{i}',sse=sse,bounds=[x,y,x+w,y+h],vertices=vertices,
                   mask=rng.getrandbits(w*h),type=rng.randrange(1,9),
                   tiles=[rng.randrange(256) for _ in range(64)] if i%2 else [],
                   rect=lo+hi,corner=vector([rng.uniform(-17000,17000),rng.uniform(-17000,17000),rng.uniform(-2000,2000)]),
                   box=[-100,-100,f32(rng.uniform(-12,0)),100,100,f32(rng.uniform(0,12))])
    for z in [-.01944445,-.019444443,-.01944444,0,.01944444,.019444443,.01944445]:
        for side in range(2):
            box=[-100,-100,-1,100,100,1];box[2+side*3]=f32(z)
            yield dict(name=f'liquid_boundary_{side}_{z}',sse=sse,bounds=[0,0,1,1],
                       vertices=[[0,0,0],[0,-1,0],[-1,0,0],[-1,-1,0]],mask=1,type=1,
                       tiles=[],rect=[0,0,7,7],corner=[0,0,0],box=box)


def flight_cases():
    rng=random.Random(12340)
    for i in range(1500):
        heights=[rng.randrange(-32768,32768) for _ in range(18)]
        z=rng.uniform(-65000,65000)
        yield dict(name=f'flight_{i}',flight_heights=heights,
                   corner=vector([rng.uniform(-17000,17000),rng.uniform(-17000,17000),0]),
                   box=[-100,-100,f32(z),100,100,f32(z+rng.uniform(0,100))])
    for z in [-32770,-32769,-32768,-2,-1,0,1,2,32766,32767,32768,65535]:
        for delta in [-.999,-.501,-.5,-.499,0,.499,.5,.501,.999]:
            yield dict(name=f'flight_cut_{z}_{delta}',flight_heights=[z//2]*18,
                       corner=[0,0,0],box=[-100,-100,f32(z+delta),100,100,f32(z+delta)])


def probe_input(c):
    if 'flight_heights' in c:
        return c['name']+' '+' '.join(map(str,c['corner']+c['box']+c['flight_heights']))+'\n'
    if 'vertices' in c:
        values=[int(c.get('sse',False)),*c['bounds'],c['mask'],c['type'],*c['rect'],*c['corner'],*c['box'],len(c['vertices'])]
        values.extend(sum(c['vertices'],[]));values.extend([len(c['tiles']),*c['tiles']])
        return c['name']+' '+' '.join(map(str,values))+'\n'
    if 'bounds' in c:
        return c['name']+' '+' '.join(map(str,c['bounds']+c['box']))+'\n'
    values=[int(c.get('sse',False)),c.get('holes',0),*c.get('rect',[0,0,7,7]),
            *c.get('corner',[0,0,0]),*c['box'],*c['heights']]
    return c['name']+' '+' '.join(map(str,values))+'\n'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dest=p.add_mutually_exclusive_group(required=True);dest.add_argument('--probe');dest.add_argument('--actual')
    mode=p.add_mutually_exclusive_group();mode.add_argument('--impassable',action='store_true');mode.add_argument('--liquid',action='store_true');mode.add_argument('--flight',action='store_true');p.add_argument('--cpu-reference');p.add_argument('--sse',action='store_true');p.add_argument('--write-reference')
    args=p.parse_args()
    if args.reference:records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_terrain_reference import OriginalTerrainQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalTerrainQuery(args.binary,args.cpu_reference)
        query=original.flight if args.flight else original.liquid if args.liquid else original.impassable if args.impassable else original.query
        inputs=flight_cases() if args.flight else liquid_cases(args.sse) if args.liquid else impassable_cases() if args.impassable else cases(args.sse)
        records=[dict(input=c,output=query(c)) for c in inputs]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMap::CreateFlightBoundsFacets 0x7a4590' if args.flight else 'CMap::GetChunkLiquidFacets 0x7a3cf0' if args.liquid else 'CMap::CreateImpassableFacets 0x7a43d0' if args.impassable else 'CMapChunk::Intersect 0x7d8840',
                                   normalization='host RSQRTSS' if args.sse else 'x87',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe]+(['--flight'] if args.flight else ['--liquid'] if args.liquid else ['--impassable'] if args.impassable else []),input=''.join(probe_input(r['input']) for r in records),
                              text=True,capture_output=True,timeout=60)
        if result.returncode:raise RuntimeError(result.stderr)
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Terrain probe dropped results')
    failures=[dict(name=r['input']['name'],expected=r['output'],actual=g)
              for r,g in zip(records,actual) if not same(r['output'],g)]
    # Limit per-case output: whole chunks contain up to 256 ordered facets.
    for f in failures[:3]:
        f['expected']['facets']=f['expected']['facets'][:2]
        f['actual']['facets']=f['actual']['facets'][:2]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:3]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
