#!/usr/bin/env python3
"""Compare geometry queries after original model/server collision-size changes."""
import argparse,json,random,subprocess,itertools
from pathlib import Path
from check_original_collision import f32,vector
from check_original_movement_query import cases as query_cases,probe_input as query_input
from check_original_facets import same as matches

def probe_input(c):
    values=[len(c['dimension_events'])]
    for e in c['dimension_events']:values.extend(e)
    return query_input(c).rstrip()+' '+' '.join(map(str,values))+'\n'

def cases():
    rng=random.Random(12340)
    for i,c in enumerate(query_cases()):
        if 1200<=i<2500:continue
        if i>=3100:break
        c['name']='dimensions_'+c['name']
        events=[]
        for j in range(rng.randrange(1,5)):
            if rng.randrange(3)==0:events.append([1,f32(rng.uniform(.1,10))])
            else:events.append([0,*vector([rng.uniform(.1,3),rng.uniform(.1,5),rng.uniform(.1,3),rng.uniform(.1,3)]),rng.randrange(2)])
        c['dimension_events']=events
        yield c
    epsilon=2**-22
    refs=[0,epsilon-epsilon*2**-23,epsilon,epsilon+epsilon*2**-23,-epsilon,1,2]
    for controlled in [False,True]:
        for force in [0,1]:
            for reference in refs:
                for sequence in range(3):
                    c=next(query_cases());c['name']=f'dimension_boundary_{controlled}_{force}_{reference}_{sequence}'
                    c['client_controlled']=controlled;c['position']=[0,0,0];c['radius']=.5;c['height']=2;c['step_height']=1
                    model=[0,1.25,3,.75,f32(reference),force]
                    c['dimension_events']=[model] if sequence==0 else ([[1,4],model] if sequence==1 else [model,[1,4],model])
                    yield c

def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--write-reference');p.add_argument('--failures')
    args=p.parse_args()
    if args.reference:records=json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_movement_query_reference import OriginalMovementQuery
        from original_collision_reference import REFERENCE_SHA256
        original=OriginalMovementQuery(args.binary)
        records=[dict(input=c,output=original.query(c)) for c in cases()]
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='SetCollisionBox 0x6e9570 / SetCollisionHeight 0x6e9600 followed by real GetMoveFacets or cache refresh',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe,'--dimensions'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode:raise RuntimeError(result.stderr)
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Query probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures:Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
