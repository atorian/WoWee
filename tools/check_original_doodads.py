#!/usr/bin/env python3
"""Compare original linked M2 selection, duplicate suppression and barriers."""
import argparse,json,math,random,subprocess
from pathlib import Path
from check_original_collision import vector
from check_original_facets import same

def cases():
    rng=random.Random(12340)
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    def obj():
        return dict(flags=0x80,static=2,tag=0,owner=0,present=True,bounds=[-2,-2,-2,2,2,2],
            model_bounds=[-1,-1,-1,1,1,1],matrix=identity.copy(),vertices=[[-1,-1,0],[1,-1,0],[0,1,0]],triangles=[[0,1,2]],normals=[[0,0,1]])
    for i in range(1000):
        objects=[]
        for j in range(rng.randrange(1,10)):
            p=obj();p['flags']=rng.choice([0,0x80,0x80,0x100,0x180]);p['tag']=rng.randrange(4);p['static']=rng.randrange(16)
            p['owner']=rng.choice([0,0,0xf130001234567890+j]);p['present']=rng.randrange(8)!=0
            if rng.randrange(10)==0:p['vertices']=[];p['triangles']=[];p['normals']=[]
            p['matrix']=vector([rng.uniform(-2,2) for _ in range(16)]);p['matrix'][3]=p['matrix'][7]=p['matrix'][11]=0;p['matrix'][15]=1
            if rng.randrange(2):p['matrix'][12:15]=vector([rng.uniform(-17000,17000) for _ in range(3)])
            if rng.randrange(3)==0:p['bounds']=[10,10,10,20,20,20]
            objects.append(p)
        calls=[]
        for k in range(rng.randrange(1,5)):
            calls.append(dict(tag=rng.randrange(4),flags=rng.choice([0,1,0xf,0x30,0x8000000f,0x100000,0xf00000,0xf0000f]),
                box=[-2,-2,-2,2,2,2],links=[rng.randrange(len(objects)) for _ in range(rng.randrange(1,15))]))
        yield dict(name=f'doodads_{i}',objects=objects,calls=calls)
    for flags in [0,1,0xf,0x30,0x100000,0xf00000,0xf0000f]:
        for prepared in [0,0x80]:
            for owner in [0,0xf130012345678901]:
                p=obj();p['flags']=prepared;p['owner']=owner
                yield dict(name=f'participation_{flags}_{prepared}_{owner}',objects=[p],calls=[dict(tag=1,flags=flags,box=[-1,-1,-1,1,1,1],links=[0,0,0])])
    for axis in range(3):
        for delta in [-.000001,0,.000001]:
            for prepared in [0,0x80]:
                p=obj();p['flags']=prepared;p['bounds']=p['model_bounds']
                box=[-1,-1,-1,1,1,1];box[axis]=1+delta
                yield dict(name=f'touch_{axis}_{delta}_{prepared}',objects=[p],calls=[dict(tag=1,flags=0xf,box=vector(box),links=[0])])
    for i in range(200):
        p=obj();p['flags']=0
        angle=rng.uniform(-math.pi,math.pi);a,b=math.cos(angle),math.sin(angle)
        center=vector([rng.uniform(-17000,17000) for _ in range(3)])
        p['matrix']=vector([a,b,0,0,-b,a,0,0,0,0,1,0,*center,1])
        box=vector([v-rng.uniform(.1,2) for v in center]+[v+rng.uniform(.1,2) for v in center])
        yield dict(name=f'placed_barrier_{i}',objects=[p],calls=[dict(tag=1,flags=0xf,box=box,links=[0,0])])
    # The first miss still marks the placement. A second list with the same
    # tag must not reconsider it; the next query tag may collect it again.
    for tag in [1,0xffffffff]:
        yield dict(name=f'repeated_query_{tag}',objects=[obj()],calls=[
            dict(tag=tag,flags=0xf,box=[10,10,10,11,11,11],links=[0]),
            dict(tag=tag,flags=0xf,box=[-1,-1,-1,1,1,1],links=[0]),
            dict(tag=(tag+1)&0xffffffff,flags=0xf,box=[-1,-1,-1,1,1,1],links=[0,0])])

def probe_input(c):
    values=[len(c['objects']),len(c['calls'])]
    for p in c['objects']:
        values.extend([p['flags'],p['static'],p['tag'],p['owner'],int(p['present']),len(p['vertices']),len(p['triangles']),
            *p['bounds'],*p['model_bounds'],*p['matrix'],*sum(p['vertices'],[]),*sum(p['triangles'],[]),*sum(p['normals'],[])])
    for q in c['calls']:values.extend([q['flags'],q['tag'],len(q['links']),*q['box'],*q['links']])
    return c['name']+' '+' '.join(map(str,values))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_doodad_reference import OriginalDoodadQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalDoodadQuery(a.binary)
        records=[dict(input=c,output=original.query(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMap::GetDoodadDefFacets 0x7a50c0 with real M2 query, TransformAABox and barriers',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines() if line.startswith('{')]
    if len(actual)!=len(records):raise RuntimeError('Missing doodad results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
