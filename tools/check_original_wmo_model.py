#!/usr/bin/env python3
"""Compare model group selection and shared WMO solid/liquid facet assembly."""
import argparse,copy,json,random,subprocess
from pathlib import Path
from check_original_collision import vector
from check_original_facets import same

def cases(sse):
    rng=random.Random(12340)
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    def group(z=0):
        return dict(ready=True,flags=0,mogi=[-10,-10,-10,10,10,10],mogp=[-20,-20,-20,20,20,20],
            vertices=[[-1,-1,z],[1,-1,z],[0,1,z]],triangles=[[0,1,2]],mopy=[8],faces=[0])
    def case(name,groups,flags=0x30,box=None):
        return dict(name=name,sse=sse,flags=flags,box=box or [-5,-5,-5,5,5,5],owner=0xf123456789abcdef,matrix=identity,groups=groups)
    for i in range(650):
        groups=[]
        for j in range(rng.randrange(1,10)):
            g=group(rng.uniform(-10,10));g['ready']=rng.randrange(4)!=0
            g['flags']=rng.choice([0,8,0x2000,0x80,0x400000,0x400080])
            start=vector([rng.uniform(-10,10) for _ in range(3)]);g['mogi']=start+vector([v+rng.uniform(.1,20) for v in start])
            g['vertices']=list(map(vector,[[rng.uniform(-10,10) for _ in range(3)] for _ in range(9)]))
            g['triangles']=[[0,1,2],[3,4,5],[6,7,8]];g['mopy']=[8,rng.choice([0,4,8,0x20,0x40,0x80]),8]
            g['faces']=rng.sample([0,1,2],3)+[0]
            if rng.randrange(2):
                w,h=rng.randrange(1,5),rng.randrange(1,5)
                g['liquid']=dict(size=[w,h],corner=vector([rng.uniform(-5,0),rng.uniform(-5,0),100]),
                    type=j+1,flags=rng.choice([0,4]),heights=vector([rng.uniform(-7,7) for _ in range((w+1)*(h+1))]),
                    tiles=[rng.choice([0,15,0x80]) for _ in range(w*h)])
            groups.append(g)
        c=case(f'model_{i}',groups,rng.choice([0x10,0x20,0x40,0x80,0x20000,0x10000,0x10030,0x20030,0x30030]))
        c['matrix']=vector([rng.uniform(-2,2) for _ in range(16)])
        c['matrix'][3]=c['matrix'][7]=c['matrix'][11]=0;c['matrix'][15]=1
        c['matrix'][12:15]=vector([rng.uniform(-17000,17000) for _ in range(3)])
        yield c
    for count in [30,31,32,33,40]:
        yield case(f'batch_limit_{count}',[group(j*.01) for j in range(count)])
    # Face/index pool capacity across large solid batches, at exact boundaries.
    for count in [8191,8192,8193]:
        g=group();g['triangles']=[[0,1,2]]*count;g['mopy']=[8]*count;g['faces']=list(range(count))
        yield case(f'face_limit_{count}',[g,copy.deepcopy(g)])
    # A sparse liquid reserves its entire clipped rectangle, not only wet cells.
    for width in [63,64]:
        g=group();w,h=width,64
        g['liquid']=dict(size=[w,h],corner=[0,0,0],type=1,flags=4,heights=[0]*((w+1)*(h+1)),tiles=[15]*(w*h))
        g['liquid']['tiles'][0]=0
        yield case(f'liquid_reservation_{width}',[g,copy.deepcopy(g),group()],0x20030,[-5,-5,-5,300,300,5])
    for flag in [0,0x80,0x400000,0x400080]:
        g=group();g['flags']=flag;g['liquid']=dict(size=[1,1],corner=[0,0,0],type=1,flags=4,heights=[0]*4,tiles=[0])
        yield case(f'group_flags_{flag}',[g],0x20030)
    for axis in range(3):
        for side in [0,3]:
            for delta in [-.0001,0,.0001]:
                g=group();box=[-5,-5,-5,5,5,5];box[axis+side]=g['mogi'][axis+3-side]+delta
                yield case(f'mogi_touch_{axis}_{side}_{delta}',[g],0x30,vector(box))

def probe_input(c):
    values=[int(c['sse']),c['flags'],c['owner'],len(c['groups']),*c['box'],*c['matrix']]
    for g in c['groups']:
        values.extend([int(g['ready']),g['flags'],*g['mogi'],*g['mogp'],len(g['vertices']),len(g['triangles']),len(g['faces'])])
        values.extend(sum(g['vertices'],[]));values.extend(sum(g['triangles'],[]));values.extend(g['mopy']);values.extend(g['faces'])
        l=g.get('liquid');values.append(int(bool(l)))
        if l:values.extend([*l['size'],l['type'],l['flags'],*l['corner'],*l['heights'],*l['tiles']])
    return c['name']+' '+' '.join(map(str,values))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--sse',action='store_true');p.add_argument('--cpu-reference');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_wmo_model_reference import OriginalWmoModel
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalWmoModel(a.binary,a.cpu_reference)
        records=[dict(input=c,output=original.query(c)) for c in cases(a.sse)]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMapObj::GetTris 0x7aef00 / GetLiquidFacets 0x7af000 + original group queries and TriDataToFacetData',normalization='host RSQRTSS' if a.sse else 'x87',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines() if line.startswith('{')]
    if len(actual)!=len(records):raise RuntimeError('Missing WMO model results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    for f in failures[:2]:
        for v in f.values():v['count']=len(v['facets']);v['facets']=v['facets'][:2]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
