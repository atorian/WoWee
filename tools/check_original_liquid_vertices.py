#!/usr/bin/env python3
"""Compare authored liquid vertex placement/format against original code."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_facets import same

def cases():
    rng=random.Random(12340)
    for i in range(1800):
        x,y=rng.randrange(8),rng.randrange(8)
        w,h=rng.randrange(1,9-x),rng.randrange(1,9-y)
        yield dict(name=f'liquid_vertices_{i}',chunk=[rng.randrange(1024),rng.randrange(1024)],
                   bounds=[x,y,x+w,y+h],base_z=f32(rng.uniform(-2000,2000)),
                   heights=vector([rng.uniform(-2000,2000) for _ in range((w+1)*(h+1))]),
                   format=i%3,legacy=bool(i%2))

def probe_input(c):
    return c['name']+' '+' '.join(map(str,[*c['chunk'],*c['bounds'],c['base_z'],c['format'],*c['heights']]))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dest=p.add_mutually_exclusive_group(required=True);dest.add_argument('--probe');dest.add_argument('--actual')
    p.add_argument('--write-reference');a=p.parse_args()
    if a.reference: records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_terrain_reference import OriginalTerrainQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalTerrainQuery(a.binary)
        records=[dict(input=c,output=original.liquid_vertices(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CChunkLiquid::CreateVertXY 0x7cdf80 and original vertex readers',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual: text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe,'--liquid-vertices'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=60)
        if run.returncode: raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Missing liquid vertex results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
