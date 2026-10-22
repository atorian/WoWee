#!/usr/bin/env python3
"""Compare unavailable terrain handling against original GetChunkFacets."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_facets import same

def cases():
    rng=random.Random(12340)
    for i in range(1200):
        x,y=rng.randrange(1024),rng.randrange(1024)
        z=rng.uniform(-2000,2000)
        low=[f32((y+1)*-33.33333206176758+17066.666015625),f32((x+1)*-33.33333206176758+17066.666015625),z]
        body=vector([low[0]+rng.uniform(-40,40),low[1]+rng.uniform(-40,40),z+rng.uniform(-2000,2000)])
        yield dict(name=f'unavailable_{i}',chunk=[x,y],loading=i%2==0,terrain=i%7!=0,
            flags=0x80000100 if i%3 else 0x100,box=vector(low+[low[0]+34,low[1]+34,z+5]),
            body=body+vector([body[0]+1,body[1]+1,body[2]+2]))
    for x,y in [(0,0),(511,512),(1023,1023)]:
        low=vector([(y+1)*-33.33333206176758+17066.666015625,(x+1)*-33.33333206176758+17066.666015625,-1000])
        high=vector([y*-33.33333206176758+17066.666015625,x*-33.33333206176758+17066.666015625,1005])
        for axis in range(3):
            for side in [0,1]:
                for delta in [-.002,0,.002]:
                    body=vector([(a+b)*.5 for a,b in zip(low,high)])
                    body[axis]=f32((low if side==0 else high)[axis]+delta)
                    yield dict(name=f'touch_{x}_{y}_{axis}_{side}_{delta}',chunk=[x,y],loading=False,
                        terrain=True,flags=0x80000100,box=[0,0,0,1,1,5],body=body+body)

def probe_input(c):
    return c['name']+' '+' '.join(map(str,[*c['chunk'],int(c['terrain']),c['flags'],*c['body'],*c['box']]))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_chunk_reference import OriginalChunkQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalChunkQuery(a.binary)
        records=[dict(input=c,output=original.unavailable(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='GetChunkFacets 0x7a5a60 unavailable area / AddAaBoxFacets 0x7a3e40',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe,'--unavailable'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=60)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing unavailable chunk query results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
