#!/usr/bin/env python3
"""Compare original 12340 authored M2 collision facet query."""
import argparse
import json
import math
from pathlib import Path
import random
import subprocess
from check_original_collision import quad, triangle, f32, vector
from check_original_fall import probe_input as fall_input
from check_original_surface import matches


def probe_input(c):
    values=[len(c['vertices']),len(c['normals']),c.get('owner',0),*c['box'],*c['matrix']]
    for v in c['vertices']: values.extend(v)
    for v in c['normals']: values.extend(v)
    for v in c['triangles']: values.extend(v)
    return c['name']+' '+' '.join(map(str,values))+'\n'


def cases():
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    def case(name, vertices, normals, triangles, matrix=identity, box=None):
        return dict(name=name,vertices=list(map(vector,vertices)),normals=list(map(vector,normals)),triangles=triangles,
                    matrix=vector(matrix),box=vector(box or [-100000]*3+[100000]*3),owner=0xf110000000001234)
    yield case('empty',[],[],[])
    for normal in [[0,0,1],[0,0,-1],[.6,.8,0],[0,0,0],[0,0,2]]:
        yield case('authored_'+str(normal[2])+'_'+str(normal[0]),[[0,0,0],[1,0,0],[0,1,0]],[normal],[[0,1,2]])
    for x in [-.000001,0,.000001,.999999,1,1.000001]:
        yield case('boundary_'+str(x),[[0,0,0],[1,0,0],[0,1,0]],[[0,0,1]],[[0,1,2]],box=[x,-1,-1,2,2,1])
    rng=random.Random(12340)
    for i in range(1500):
        vertices=[[rng.uniform(-10,10) for _ in range(3)] for _ in range(6)]
        normals=[[rng.uniform(-1,1) for _ in range(3)] for _ in range(3)]
        matrix=[rng.uniform(-3,3) for _ in range(16)];matrix[3]=matrix[7]=matrix[11]=0;matrix[15]=1
        matrix[12:15]=[rng.uniform(-10000,10000) for _ in range(3)]
        if i%4==0:matrix=identity
        if i%10==0:matrix[:3]=[0,0,0]
        box=[matrix[12+k]-rng.uniform(0,20) for k in range(3)]+[matrix[12+k]+rng.uniform(0,20) for k in range(3)]
        yield case(f'm2_{i}',vertices,normals,[[0,1,2],[3,4,5],[2,1,0]],matrix,box)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    source=p.add_mutually_exclusive_group(required=True);source.add_argument('--binary');source.add_argument('--reference')
    output=p.add_mutually_exclusive_group(required=True);output.add_argument('--probe');output.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures')
    args=p.parse_args()
    if args.reference: records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_collision_reference import REFERENCE_SHA256
        from original_m2_reference import OriginalM2Query
        original=OriginalM2Query(args.binary)
        records=[dict(input=c, output=original.query(c)) for c in cases()]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CM2Model::GetCollisionFacets 0x82ec30',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual: text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode: raise RuntimeError(f'M2 probe failed: {result.returncode}: {result.stderr}')
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records): raise RuntimeError('M2 probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures: Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=[dict(name=f['input']['name'],expected=f['expected'],actual=f['actual']) for f in failures[:3]]),indent=2))
    return bool(failures)


if __name__=='__main__': raise SystemExit(main())
