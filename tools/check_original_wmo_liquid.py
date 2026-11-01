#!/usr/bin/env python3
"""Compare original WMO liquid generation, culling, order and transformed facets."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_facets import same

def cases(sse):
    rng=random.Random(12340)
    for i in range(850):
        w,h=rng.randrange(1,13),rng.randrange(1,13)
        corner=vector([rng.uniform(-1000,1000),rng.uniform(-1000,1000),rng.uniform(-1000,1000)])
        matrix=vector([rng.uniform(-3,3) for _ in range(16)])
        matrix[3]=matrix[7]=matrix[11]=0;matrix[15]=1
        matrix[12:15]=vector([rng.uniform(-17000,17000) for _ in range(3)])
        box=vector([corner[0]+rng.uniform(-10,30),corner[1]+rng.uniform(-10,30),rng.uniform(-20,0),
                    corner[0]+rng.uniform(-10,60),corner[1]+rng.uniform(-10,60),rng.uniform(0,20)])
        yield dict(name=f'wmo_liquid_{i}',sse=sse,size=[w,h],corner=corner,box=box,matrix=matrix,
                   flags=rng.choice([0x10000,0x20000,0x30000]),liquid_flags=rng.choice([0,4]),type=i%20,
                   owner=rng.getrandbits(64),tiles=[rng.randrange(256) for _ in range(w*h)],
                   heights=vector([rng.uniform(-25,25) for _ in range((w+1)*(h+1))]))
    for axis in [0,1]:
        for z in [-.01944445,-.019444443,-.01944444,0,.01944444,.019444443,.01944445]:
            box=[-1,-1,-1,10,10,1];box[2+axis*3]=f32(z)
            yield dict(name=f'wmo_margin_{axis}_{z}',sse=sse,size=[1,1],corner=[0,0,0],box=box,
                matrix=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],flags=0x20000,liquid_flags=4,type=1,owner=17,
                tiles=[0],heights=[0]*4)

    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    for w,h in [(89,90),(90,90)]:
        tiles=[15]*(w*h);tiles[-1]=0
        yield dict(name=f'wmo_cull_limit_{w}_{h}',sse=sse,size=[w,h],corner=[0,0,0],
            box=[-1,-1,-1,500,500,1],matrix=identity,flags=0x20000,liquid_flags=4,type=1,owner=0,
            tiles=tiles,heights=[0]*((w+1)*(h+1)))
    for n in [0,1,2,7,8,11,12]:
        for delta in [-.00001,0,.00001]:
            edge=f32(n*4.166666507720947+delta)
            for axis in [0,1]:
                box=[0,0,-1,4,4,1];box[axis]=edge;box[axis+3]=edge
                yield dict(name=f'wmo_cell_{axis}_{n}_{delta}',sse=sse,size=[12,12],corner=[0,0,0],
                    box=box,matrix=identity,flags=0x20000,liquid_flags=4,type=1,owner=0,
                    tiles=[0]*144,heights=[0]*169)

def probe_input(c):
    return c['name']+' '+' '.join(map(str,[int(c['sse']),*c['size'],*c['corner'],*c['box'],c['flags'],c['liquid_flags'],c['type'],c['owner'],*c['matrix'],*c['heights'],*c['tiles']]))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dest=p.add_mutually_exclusive_group(required=True);dest.add_argument('--probe');dest.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--sse',action='store_true');p.add_argument('--cpu-reference');a=p.parse_args()
    if a.reference: records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_wmo_liquid_reference import OriginalWmoLiquid
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalWmoLiquid(a.binary,a.cpu_reference)
        records=[dict(input=c,output=original.query(c)) for c in cases(a.sse)]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMapObjGroup::GenLiquidVerts 0x7c8c60 / GetLiquidFacets 0x7ca110',normalization='host RSQRTSS' if a.sse else 'x87',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual: text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=60)
        if run.returncode: raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing WMO liquid query results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    for f in failures[:2]:
        f['expected']['facets']=f['expected']['facets'][:2];f['actual']['facets']=f['actual']['facets'][:2]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
