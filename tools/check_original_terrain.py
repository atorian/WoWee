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


def probe_input(c):
    values=[int(c.get('sse',False)),c.get('holes',0),*c.get('rect',[0,0,7,7]),
            *c.get('corner',[0,0,0]),*c['box'],*c['heights']]
    return c['name']+' '+' '.join(map(str,values))+'\n'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dest=p.add_mutually_exclusive_group(required=True);dest.add_argument('--probe');dest.add_argument('--actual')
    p.add_argument('--cpu-reference');p.add_argument('--sse',action='store_true');p.add_argument('--write-reference')
    args=p.parse_args()
    if args.reference:records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_terrain_reference import OriginalTerrainQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalTerrainQuery(args.binary,args.cpu_reference)
        records=[dict(input=c,output=original.query(c)) for c in cases(args.sse)]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMapChunk::Intersect 0x7d8840',
                                   normalization='host RSQRTSS' if args.sse else 'x87',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe],input=''.join(probe_input(r['input']) for r in records),
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
