#!/usr/bin/env python3
"""Compare dynamic entity selection, callback parameters, ownership and facets."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import vector
from check_original_facets import same

def cases():
    rng=random.Random(12340)
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    def obj(index):
        owner=0xf110000012345678+index
        return dict(static=2,tag=0,owner=owner,parameter=index,available=True,returned_owner=owner,
            bounds=[-2,-2,-2,2,2,2],matrix=identity.copy(),vertices=[[-1,-1,0],[1,-1,0],[0,1,0]],triangles=[[0,1,2]],normals=[[0,0,1]])
    for i in range(1000):
        objects=[]
        for j in range(rng.randrange(1,10)):
            p=obj(j);p['static']=rng.choice([0,1,2,2,2,3,6,10]);p['tag']=rng.randrange(4)
            p['available']=rng.randrange(4)!=0
            if rng.randrange(4)==0:p['returned_owner']+=10000
            if rng.randrange(10)==0:p['vertices']=[];p['triangles']=[];p['normals']=[]
            p['matrix']=vector([rng.uniform(-2,2) for _ in range(16)]);p['matrix'][3]=p['matrix'][7]=p['matrix'][11]=0;p['matrix'][15]=1
            if rng.randrange(3)==0:p['bounds']=[10,10,10,20,20,20]
            objects.append(p)
        calls=[]
        for k in range(rng.randrange(1,5)):
            calls.append(dict(tag=rng.randrange(4),provider=rng.randrange(6)!=0,flags=rng.choice([0,0xf,0x30,0x100000,0xf00000,0xf0000f]),
                box=[-2,-2,-2,2,2,2],links=[rng.randrange(len(objects)) for _ in range(rng.randrange(1,15))]))
        yield dict(name=f'entities_{i}',objects=objects,calls=calls)
    for flags in [0,0xf,0x30,0x100000,0xf00000,0xf0000f]:
        for static in [0,1,2,3,4,6,10]:
            for available in [False,True]:
                objects=[obj(0),obj(1),obj(2)];objects[1]['static']=static;objects[1]['available']=available
                yield dict(name=f'participation_{flags}_{static}_{available}',objects=objects,calls=[dict(tag=1,provider=True,flags=flags,box=[-1,-1,-1,1,1,1],links=[0,1,2,0])])
    for axis in range(3):
        for delta in [-.000001,0,.000001]:
            box=[-1,-1,-1,1,1,1];box[axis]=2+delta
            yield dict(name=f'touch_{axis}_{delta}',objects=[obj(0)],calls=[dict(tag=1,provider=True,flags=0xf00000,box=vector(box),links=[0])])
    for tag in [1,0xffffffff]:
        p=obj(0);p['available']=False
        yield dict(name=f'failed_callback_{tag}',objects=[p],calls=[dict(tag=t,provider=True,flags=0xf00000,box=[-1,-1,-1,1,1,1],links=[0,0]) for t in [tag,tag,(tag+1)&0xffffffff]])

def probe_input(c):
    values=[len(c['objects']),len(c['calls'])]
    for p in c['objects']:
        values.extend([p['static'],p['tag'],p['owner'],p['parameter'],int(p['available']),p['returned_owner'],len(p['vertices']),len(p['triangles']),
            *p['bounds'],*p['matrix'],*sum(p['vertices'],[]),*sum(p['triangles'],[]),*sum(p['normals'],[])])
    for q in c['calls']:values.extend([q['flags'],q['tag'],int(q['provider']),len(q['links']),*q['box'],*q['links']])
    return c['name']+' '+' '.join(map(str,values))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_entity_reference import OriginalEntityQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalEntityQuery(a.binary)
        records=[dict(input=c,output=original.query(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMap::QueryDestructibleFacets 0x7a5240 with fixture collision callback and real M2 query',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines() if line.startswith('{')]
    if len(actual)!=len(records):raise RuntimeError('Missing entity results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
