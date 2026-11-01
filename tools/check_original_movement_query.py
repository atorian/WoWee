#!/usr/bin/env python3
"""Compare original mover query extents and geometry participation flags."""
import argparse,json,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_swim import probe_input as swim_input
from check_original_surface import matches


def probe_input(c):
    extra=[int(c['local']),int(c['ghost']),int('cached_box' in c),*c.get('cached_box',[])]
    return swim_input(c).rstrip()+' '+' '.join(map(str,extra))+'\n'


def cases():
    rng=random.Random(12340)
    for i in range(2500):
        flags=rng.choice([1,0x1001,0x20001001,0x200001,0x2000001,0x40000001,0x10000001,0x12000001])
        pos=vector([rng.uniform(-16000,16000),rng.uniform(-16000,16000),rng.uniform(-1000,1000)])
        yield dict(name=f'query_{i}',radius=f32(rng.uniform(.01,3)),height=f32(rng.uniform(.1,10)),
                   step_height=f32(rng.uniform(0,3)),client_controlled=bool(i%3),local=bool(i%2),ghost=bool(i%5==0 and i%3),
                   position=pos,flags=flags,flags2=rng.choice([0,0x200,0x4000]),pitch=f32(rng.choice([-.6457718,-.6457717,-.6457719,0])),
                   distance=f32(rng.uniform(0,10)),elapsed=rng.randrange(1,1001),direction=[1,0],
                   swim_direction=vector([rng.uniform(-1,1) for _ in range(3)]),triangles=[],liquids=[],
                   fall_time=rng.randrange(0,3000),fall_start=f32(pos[2]+rng.uniform(-10,10)),
                   fall_velocity=f32(rng.choice([0,-7.9555473,10])))

    for i in range(1200):
        position=vector([rng.uniform(-16000,16000),rng.uniform(-16000,16000),rng.uniform(-1000,1000)])
        radius=f32(rng.uniform(.1,2));height=f32(rng.uniform(.5,8))
        box=vector([position[0]-radius-.2,position[1]-radius-.2,position[2]-.2,
                    position[0]+radius+.2,position[1]+radius+.2,position[2]+height+.2])
        move=vector([rng.uniform(-1,1)*(0.1 if i%2 else 10) for _ in range(3)])
        yield dict(name=f'refresh_{i}',radius=radius,height=height,position=position,
                   direction=[1,0],swim_direction=move,distance=1,elapsed=16,triangles=[],liquids=[],
                   flags=rng.choice([1,0x1001,0x200001,0x2000001,0x10000001]),
                   flags2=0,local=bool(i%2),client_controlled=True,ghost=bool(i%3==0),cached_box=box)


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
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='GetMoveFacets 0x75ff90',fpu_control=0x27f),indent=2)
            rows=',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:text=Path(args.actual).read_text()
    else:
        result=subprocess.run([args.probe,'--query'],input=''.join(probe_input(r['input']) for r in records),text=True,capture_output=True,timeout=90)
        if result.returncode:raise RuntimeError(result.stderr)
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(actual)!=len(records):raise RuntimeError('Query probe dropped results')
    failures=[dict(input=r['input'],expected=r['output'],actual=g) for r,g in zip(records,actual) if not matches(r['output'],g)]
    if args.failures:Path(args.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(records),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
