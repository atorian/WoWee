#!/usr/bin/env python3
"""Compare authored WMO BSP traversal against build 12340, without a game."""
import argparse
import json
from pathlib import Path
import random
import subprocess
from check_original_collision import vector


def cases():
    def leaf(name, triangles=1, cache=True):
        return dict(name=name, nodes=[[4,65535,65535,triangles,0,0]],
                    faces=list(range(triangles)), vertices=[[-1,-1,0],[1,-1,0],[0,1,0]],
                    triangles=[[0,1,2]]*triangles, bounds=[-5,-5,-5,5,5,5],
                    box=[-2,-2,-1,2,2,1], cache=cache)
    for cache in [False,True]:
        for axis in range(3):
            for side in [0,1]:
                for edge in [-1.001,-1,-.999,0,.999,1,1.001]:
                    c=leaf(f'boundary_{cache}_{axis}_{side}_{edge}',cache=cache)
                    c['box'][axis+3*side]=edge
                    if c['box'][axis]<=c['box'][axis+3]:yield c
        for flags in [0x100111,0x100122,0x100140,0x100180,0x100040,0x1004020,0x41000000]:
            c=leaf(f'mask_{cache}_{flags}',16,cache)
            c['query_flags']=flags
            c['triangle_flags']=[0,1,2,4,8,16,32,64,128,255,0x24,0x28,0x48,0xc0,0x84,0x20]
            yield c
        for count in [300,301,8192,8193]:
            yield leaf(f'capacity_{cache}_{count}',count,cache)
        for count in [150,151]:
            c=leaf(f'digest_vertices_{cache}_{count}',count,cache)
            c['vertices']=[v for _ in range(count) for v in [[-1,-1,0],[1,-1,0],[0,1,0]]]
            c['triangles']=[[i*3,i*3+1,i*3+2] for i in range(count)]
            c['box'][2]=0
            yield c
        c=leaf(f'duplicate_boundary_{cache}',302,cache)
        c['nodes']=[[0,2,1,0,0,0],[4,65535,65535,1,0,0],[4,65535,65535,301,1,0]]
        c['faces']=[0,0]+list(range(1,301))
        c['box'][2]=0
        yield c
    rng=random.Random(12340)
    for i in range(400):
        count=rng.randrange(1,40)
        vertices=[vector([rng.uniform(-5,5) for _ in range(3)]) for _ in range(count*3)]
        order=list(range(count));rng.shuffle(order)
        cut=rng.randrange(count+1)
        c=leaf(f'tree_{i}',count,bool(i%2))
        c['vertices']=vertices
        c['triangles']=[[j*3,j*3+1,j*3+2] for j in range(count)]
        c['nodes']=[[i%3,2,1,0,0,rng.uniform(-3,3)],
                    [4,65535,65535,cut,0,0],[4,65535,65535,count-cut,cut,0]]
        c['faces']=order
        c['triangle_flags']=[rng.choice([0,4,8,0x20,0x24]) for _ in range(count)]
        low=vector([rng.uniform(-5,2) for _ in range(3)])
        high=vector([low[j]+rng.uniform(.001,5) for j in range(3)])
        c['box']=low+high
        yield c


def probe_input(c):
    values=[len(c['vertices']),len(c['triangles']),len(c['nodes']),len(c['faces']),
            c.get('query_flags',0x100111),int(c.get('cache',True)),*c['bounds'],*c['box']]
    for v in c['vertices']:values.extend(v)
    for t in c['triangles']:values.extend(t)
    values.extend(c.get('triangle_flags',[0]*len(c['triangles'])))
    for n in c['nodes']:values.extend(n)
    values.extend(c['faces'])
    return c['name']+' '+' '.join(map(str,values))+'\n'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    source=parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--binary');source.add_argument('--reference')
    output=parser.add_mutually_exclusive_group(required=True)
    output.add_argument('--probe');output.add_argument('--actual')
    parser.add_argument('--write-reference')
    args=parser.parse_args()
    if args.reference:
        records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_wmo_reference import OriginalWmoQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalWmoQuery(args.binary)
        records=[dict(input=c,output=original.query(c)) for c in cases()]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='WMO BSP 0x7ca920'),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:
        text=Path(args.actual).read_text()
    else:
        p=subprocess.run([args.probe],input=''.join(probe_input(r['input']) for r in records),
                         capture_output=True,text=True,timeout=60)
        if p.returncode:raise RuntimeError(f'WMO probe failed ({p.returncode}):\n{p.stderr}')
        text=p.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('WMO probe dropped results')
    failures=[dict(name=r['input']['name'],expected=r['output'],actual=g)
              for r,g in zip(records,actual) if r['output']!=g]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:6]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
