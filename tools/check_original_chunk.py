#!/usr/bin/env python3
"""Compare loaded chunk terrain/liquid assembly with original machine code."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_facets import same

def cases():
    rng=random.Random(12340)
    for i in range(650):
        x,y=rng.randrange(1024),rng.randrange(1024)
        base=f32(rng.uniform(-1000,1000))
        corner=vector([17066.666015625-y*33.33333206176758,17066.666015625-x*33.33333206176758,base])
        liquids=[]
        for j in range(i%3):
            legacy=(i+j)%2==0
            a,b=(0,0) if legacy else (rng.randrange(8),rng.randrange(8))
            w,h=(8,8) if legacy else (rng.randrange(1,9-a),rng.randrange(1,9-b))
            liquids.append(dict(bounds=[a,b,a+w,b+h],legacy=legacy,type=1+j*2,flags=4 if (i+j)%3 else 0,
                format=(i+j)%3,heights=vector([base+rng.uniform(-10,10) for _ in range((w+1)*(h+1))]),
                mask=rng.getrandbits(w*h),tiles=[rng.choice([0,2,0x80,0x82,15]) for _ in range(64)] if legacy else []))
        lo=[rng.randrange(8),rng.randrange(8)];hi=[rng.randrange(lo[j],8) for j in range(2)]
        yield dict(name=f'chunk_{i}',chunk=[x,y],base_z=base,map_id=530 if i%7==0 else 0,
            heights=vector([rng.uniform(-20,20) for _ in range(145)]),holes=rng.randrange(65536),
            impassable=bool(i%3==0),rect=[lo[0]+x*8,lo[1]+y*8,hi[0]+x*8,hi[1]+y*8],
            flags=rng.choice([0,0x100,0x10000,0x20000,0x30100,0x10100]),liquids=liquids,
            box=vector([corner[0]-rng.uniform(20,40),corner[1]-rng.uniform(20,40),base-30,
                        corner[0]+rng.uniform(-10,1),corner[1]+rng.uniform(-10,1),base+30]))

def probe_input(c):
    values=[*c['chunk'],c['base_z'],c['map_id'],c['holes'],int(c['impassable']),c['flags'],*c['rect'],*c['box'],*c['heights'],len(c['liquids'])]
    for l in c['liquids']:
        values.extend([int(l['legacy']),l['type'],l['flags'],l['format'],*l['bounds'],l['mask'],*l['heights'],*l['tiles']])
    return c['name']+' '+' '.join(map(str,values))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dest=p.add_mutually_exclusive_group(required=True);dest.add_argument('--probe');dest.add_argument('--actual')
    p.add_argument('--write-reference');a=p.parse_args()
    if a.reference: records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_chunk_reference import OriginalChunkQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalChunkQuery(a.binary)
        records=[dict(input=c,output=original.query(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMap::GetChunkFacets 0x7a5a60, loaded chunk with empty object lists',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual: text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=60)
        if run.returncode: raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing chunk query results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    for f in failures[:2]:
        f['expected']['facets']=f['expected']['facets'][:2];f['actual']['facets']=f['actual']['facets'][:2]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
