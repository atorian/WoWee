#!/usr/bin/env python3
"""Compare original world chunk selection, visitation order and query failures."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector

def cases():
    rng=random.Random(12340)
    for i in range(2000):
        low=[rng.uniform(-17066,17066),rng.uniform(-17066,17066),-100]
        high=[low[0]+rng.uniform(0,100),low[1]+rng.uniform(0,100),100]
        yield dict(name=f'map_{i}',box=vector(low+high),dungeon=i%13==0,
                   failed=rng.choice([-1,0,1,2,3,4,8,20]),flags=rng.choice([0x100111,0x80010311,0x20000,0x200]))
    for axis in [0,1]:
        for n in [0,1,7,8,15,16,127,128,4095,4096,8183,8191,8192]:
            for delta in [-.002,-.0001,0,.0001,.002]:
                v=f32(17066.666015625-n*4.166666507720947+delta)
                box=[0,0,-10,0,0,10];box[axis]=v;box[axis+3]=v
                yield dict(name=f'seam_{axis}_{n}_{delta}',box=box,dungeon=False,failed=-1,flags=0x200)

def probe_input(c):
    return c['name']+' '+' '.join(map(str,[int(c['dungeon']),c['failed'],c['flags'],*c['box']]))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dest=p.add_mutually_exclusive_group(required=True);dest.add_argument('--probe');dest.add_argument('--actual')
    p.add_argument('--write-reference');a=p.parse_args()
    if a.reference: records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_map_reference import OriginalMapQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMapQuery(a.binary)
        records=[dict(input=c,output=original.query(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMap::GetFacets 0x7a5f20',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual: text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=60)
        if run.returncode: raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing map query results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if r['output']!=g]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
