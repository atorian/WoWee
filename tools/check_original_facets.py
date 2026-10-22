#!/usr/bin/env python3
"""Check transformed collision planes/vertices against actual build 12340."""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
from check_original_collision import vector


def cases(sse):
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    def case(name, vertices, matrix=identity, triangles=None):
        return dict(name=name,sse=sse,owner=0xf123456789abcdef,batches=[dict(
            matrix=vector(matrix),vertices=list(map(vector,vertices)),triangles=triangles or [[0,1,2]])])
    yield dict(name='empty',sse=sse,batches=[])
    for size in [0,1e-6,.1,1,2,10,1000]:
        yield case(f'floor_{size}',[[0,0,0],[size,0,0],[0,size,0]])
        yield case(f'reversed_{size}',[[0,0,0],[0,size,0],[size,0,0]])
    yield case('degenerate_line',[[0,0,0],[1,2,3],[2,4,6]])
    rng=random.Random(12340)
    for i in range(1000):
        verts=[[rng.uniform(-100,100) for _ in range(3)] for _ in range(6)]
        # Include general finite transforms to catch reassociation and local vs
        # transformed cross products; real placements are a subset of these.
        matrix=[rng.uniform(-3,3) for _ in range(16)]
        matrix[3]=matrix[7]=matrix[11]=0;matrix[15]=1
        matrix[12:15]=[rng.uniform(-17000,17000) for _ in range(3)]
        if i%4==0:matrix=identity
        yield case(f'transform_{i}',verts,matrix,[[0,1,2],[3,4,5],[2,1,0]])
    c=case('batches',[[0,0,0],[1,0,0],[0,1,0]])
    c['batches']*=3
    yield c


def probe_input(c):
    values=[int(c.get('sse',False)),c.get('owner',0),len(c['batches'])]
    for b in c['batches']:
        values.extend([len(b['vertices']),len(b['triangles']),*b['matrix']])
        for v in b['vertices']:values.extend(v)
        for t in b['triangles']:values.extend(t)
    return c['name']+' '+' '.join(map(str,values))+'\n'


def same(a,b):
    if isinstance(a,float):return struct.pack('<f',a)==struct.pack('<f',b)
    if isinstance(a,list):return len(a)==len(b) and all(same(x,y) for x,y in zip(a,b))
    if isinstance(a,dict):return a.keys()==b.keys() and all(same(a[k],b[k]) for k in a)
    return a==b


def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dest=p.add_mutually_exclusive_group(required=True);dest.add_argument('--probe');dest.add_argument('--actual')
    p.add_argument('--cpu-reference');p.add_argument('--sse',action='store_true');p.add_argument('--write-reference')
    args=p.parse_args()
    if args.reference:
        records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_facet_reference import OriginalFacetBuilder
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalFacetBuilder(args.binary,args.cpu_reference)
        records=[dict(input=c,output=original.build(c)) for c in cases(args.sse)]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='TriDataToFacetData 0x782740',
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
    if len(actual)!=len(records):raise RuntimeError('Facet probe dropped results')
    failures=[dict(name=r['input']['name'],expected=r['output'],actual=g)
              for r,g in zip(records,actual) if not same(r['output'],g)]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:3]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
