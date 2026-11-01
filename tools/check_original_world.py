#!/usr/bin/env python3
"""Compare the complete supplied-state world collection, including shared links."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_facets import same

def cases():
    rng=random.Random(12340)
    masks=[0,0xf,0x30,0x100,0x30000,0x200,0xf0000f,0x8000013f,0x8033033f]
    for i in range(1000):
        yield dict(name=f'complete_world_{i}',dungeon=rng.randrange(4)==0,wmo=rng.randrange(6),area=rng.randrange(6),
            doodad_flags=rng.choice([0,0x80,0x100,0x180]),owner=rng.choice([0,0xf110001234567890]),entity_flags=rng.choice([0,2,3,6]),
            provider=rng.randrange(4)!=0,available=rng.randrange(4)!=0,impassable=rng.randrange(4)==0,holes=rng.choice([0,0xffff,0x50]),
            flight=rng.randrange(2)!=0,tag=rng.choice([0,99,0xffffffff]),flags=[rng.choice(masks) for _ in range(3)],
            body=[100,100,100,101,101,101] if i%2 else [-5,-34,-2,-3,-32,5],box=[-5,-34,-2,-3,-32,5])
    for wmo in range(6):
        for area in range(6):
            for owner in [0,0xf110001234567890]:
                yield dict(name=f'shared_links_{wmo}_{area}_{owner}',dungeon=False,wmo=wmo,area=area,doodad_flags=0x80,owner=owner,
                    entity_flags=2,provider=True,available=True,impassable=False,holes=0,flight=True,tag=0,
                    flags=[0x8033033f,0x8033033f,0x30],body=[-5,-34,-2,-3,-32,5],box=[-5,-34,-2,-3,-32,5])

def probe_input(c):
    values=[int(c['dungeon']),c['wmo'],c['area'],c['doodad_flags'],c['owner'],c['entity_flags'],int(c['provider']),int(c['available']),
        int(c['impassable']),c['holes'],int(c['flight']),c['tag'],len(c['flags']),*c['body'],*c['box'],*c['flags']]
    return c['name']+' '+' '.join(map(str,values))+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');a=p.parse_args()
    if a.reference:records=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_world_reference import OriginalWorldQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalWorldQuery(a.binary)
        records=[dict(input=c,output=original.query(c)) for c in cases()]
        if a.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='CMap::GetFacets 0x7a5f20 with real WMO, terrain, linked M2 and entity collectors; supplied entity collision callback',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(r) for r in records)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        run=subprocess.run([a.probe],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr)
        text=run.stdout
    actual=[json.loads(line) for line in text.splitlines() if line.startswith('{')]
    if len(actual)!=len(records):raise RuntimeError('Missing complete world results')
    failures=[dict(expected=r['output'],actual=g) for r,g in zip(records,actual) if not same(r['output'],g)]
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
