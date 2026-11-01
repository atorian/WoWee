#!/usr/bin/env python3
"""Compare original world WMO order, transforms, readiness and linked calls."""
import argparse,copy,json,math,random,subprocess
from pathlib import Path
from check_original_collision import vector
from check_original_facets import same
from check_original_wmo_model import probe_input as model_input

def cases(sse):
    rng=random.Random(12340)
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    def placement(owner=100):
        groups=[dict(ready=True,flags=0,mogi=[-2,-2,-2,2,2,2],mogp=[-3,-3,-3,3,3,3],
            vertices=[[-1,-1,z],[1,-1,z],[0,1,z]],triangles=[[0,1,2]],mopy=[8],faces=[0]) for z in [-.5,0,.5]]
        groups[1]['liquid']=dict(size=[1,1],type=1,flags=4,corner=[0,0,0],heights=[.1,.2,.3,.4],tiles=[0])
        return dict(flags=0,present=True,ready=True,owner=owner,bounds=[-5,-5,-5,5,5,5],root=[-3,-3,-3,3,3,3],
            matrix=identity.copy(),inverse=identity.copy(),groups=groups,
            links=[dict(index=i,id=owner*10+i,ready=True,bounds=[-5,-5,-5,5,5,5]) for i in [2,0,1]])
    def case(name,placements=None):
        return dict(name=name,sse=sse,flags=0x8023003f,status=8,body=[-.1,-.1,-.1,.1,.1,.1],box=[-1,-1,-1,1,1,1],
            proxies=[],placements=[placement()] if placements is None else placements)
    for i in range(1000):
        c=case(f'world_{i}',[placement(j+100) for j in range(rng.randrange(1,6))])
        c['flags']=rng.choice([0,0x30,0x30000,0xf0000f,0x80000030,0x8000000f,0x8023003f])
        c['body']=vector([50,50,50,51,51,51]) if i%2 else c['body']
        for p in c['placements']:
            p['flags']=rng.choice([0,0,0x80,0x100]);p['present']=rng.randrange(10)!=0;p['ready']=rng.randrange(8)!=0
            angle=rng.uniform(-math.pi,math.pi);scale=rng.uniform(.5,2)
            a,b=math.cos(angle)*scale,math.sin(angle)*scale
            p['matrix']=vector([a,b,0,0,-b,a,0,0,0,0,scale,0,0,0,0,1])
            p['inverse']=vector([a/scale**2,-b/scale**2,0,0,b/scale**2,a/scale**2,0,0,0,0,1/scale,0,0,0,0,1])
            p['inverse'][12:15]=vector([rng.uniform(-2,2) for _ in range(3)])
            p['matrix'][12:15]=vector([rng.uniform(-17000,17000) for _ in range(3)])
            for g in p['groups']:
                g['ready']=rng.randrange(6)!=0;g['flags']=rng.choice([0,0,0x80,0x400000])
                g['mogi']=vector([-.75,-.75,-.75,.75,.75,.75]) if rng.randrange(2) else g['mogi']
            for g in p['links']:
                g['ready']=rng.randrange(20)!=0
                if rng.randrange(4)==0:g['bounds']=[10,10,10,20,20,20]
            rng.shuffle(p['links'])
            if rng.randrange(12)==0:p['links']=[]
            if rng.randrange(10)==0:p['bounds']=[10,10,10,20,20,20]
            if rng.randrange(8)==0:p['root']=[10,10,10,20,20,20]
        if i%8==0:c['proxies']=[vector([rng.uniform(-10,10) for _ in range(3)]+[rng.uniform(0,2)])]
        yield c
    # Consistent world placements: rotated/scaled models, translated queries,
    # and matching inverse matrices rather than independent matrix stress.
    for i in range(300):
        c=case(f'placed_{i}');p=c['placements'][0]
        center=vector([rng.uniform(-17000,17000) for _ in range(3)])
        angle=rng.uniform(-math.pi,math.pi);scale=rng.uniform(.1,4)
        a,b=math.cos(angle)*scale,math.sin(angle)*scale
        p['matrix']=vector([a,b,0,0,-b,a,0,0,0,0,scale,0,*center,1])
        p['inverse']=vector([a/scale**2,-b/scale**2,0,0,b/scale**2,a/scale**2,0,0,0,0,1/scale,0,
            -(a*center[0]+b*center[1])/scale**2,(b*center[0]-a*center[1])/scale**2,-center[2]/scale,1])
        p['bounds']=vector([v-20 for v in center]+[v+20 for v in center])
        for g in p['links']:g['bounds']=p['bounds']
        c['box']=vector([v-rng.uniform(.1,2) for v in center]+[v+rng.uniform(.1,2) for v in center])
        c['body']=vector([v-.1 for v in center]+[v+.1 for v in center])
        yield c
    for part in ['root_ready','no_links','group_ready']:
        for local in [0,0x80000000]:
            for body in [[-.1,-.1,-.1,.1,.1,.1],[10,10,10,11,11,11],[5,0,0,6,1,1],[5.000001,0,0,6,1,1]]:
                c=case(f'{part}_{local}_{body[0]}',[placement(100),placement(200),placement(300)])
                c['flags']=local|0x3003f;c['body']=body;p=c['placements'][1]
                if part=='root_ready':p['ready']=False
                elif part=='no_links':p['links']=[]
                else:p['groups'][0]['ready']=False
                yield c
    for axis in range(3):
        for coord in [-17000,-1,0,1,17000]:
            for delta in [-.000001,0,.000001,.001]:
                c=case(f'boundary_{axis}_{coord}_{delta}')
                c['box']=vector([coord-.3]*3+[coord+.3]*3)
                p=c['placements'][0];p['bounds']=[-20000]*3+[20000]*3
                p['inverse'][12:15]=vector([-coord]*3)
                p['root'][axis]=.3+delta
                yield c
    for axis in range(3):
        for delta in [-.000001,0,.000001]:
            c=case(f'proxy_touch_{axis}_{delta}');c['box']=[0]*6
            proxy=[0,0,0,1];proxy[axis]=1+delta;c['proxies']=[vector(proxy)]
            yield c
    # Large world coordinates with tiny extents exercise the unrounded center.
    for i in range(300):
        c=case(f'precision_{i}');p=c['placements'][0]
        center=vector([rng.uniform(-17000,17000) for _ in range(3)])
        c['box']=vector([v-rng.uniform(.0001,2) for v in center]+[v+rng.uniform(.0001,2) for v in center])
        p['bounds']=[-20000]*3+[20000]*3
        p['inverse']=vector([rng.uniform(-2,2) for _ in range(16)])
        p['inverse'][3]=p['inverse'][7]=p['inverse'][11]=0;p['inverse'][15]=1
        for a in range(3):p['inverse'][12+a]=vector([-sum(p['inverse'][k*4+a]*center[k] for k in range(3))])[0]
        yield c

def probe_input(c):
    values=[int(c['sse']),c['flags'],c['status'],len(c['placements']),len(c['proxies']),*c['body'],*c['box'],*sum(c['proxies'],[])]
    for p in c['placements']:
        values.extend([p['flags'],int(p['present']),int(p['ready']),p['owner'],len(p['groups']),len(p['links']),*p['bounds'],*p['root'],*p['matrix'],*p['inverse']])
        m=dict(name='unused',sse=False,flags=0,owner=0,groups=p['groups'],box=[0]*6,matrix=[0]*16)
        values.extend(model_input(m).split()[27:])
        for g in p['links']:values.extend([g['index'],g['id'],int(g['ready']),*g['bounds']])
    return c['name']+' '+' '.join(map(str,values))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--sse',action='store_true');p.add_argument('--cpu-reference');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_wmo_world_reference import OriginalWmoWorld
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalWmoWorld(a.binary,a.cpu_reference)
        records=[dict(input=c,output=original.query(c)) for c in cases(a.sse)]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMap::GetMapObjFacets 0x7a55e0 with real models/barriers; linked doodad/entity service stubs recorded',normalization='host RSQRTSS' if a.sse else 'x87',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines() if line.startswith('{')]
    if len(actual)!=len(records):raise RuntimeError('Missing world WMO results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
