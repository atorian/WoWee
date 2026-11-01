#!/usr/bin/env python3
"""Compare original input/time projection before collision requests."""
import argparse,json,math,random,subprocess
from pathlib import Path
from check_original_collision import f32,vector
from check_original_surface import matches

def probe_input(c):
    v=[c['elapsed'],c['flags'],c['flags2'],c['speed'],c['turn_rate'],c['pitch_rate'],c['anchor_facing'],c['anchor_pitch'],
       *c['direction'],*c['direction2d'],c['pitch_cos'],c['pitch_sin'],*c['move'],c['facing'],c['pitch'],int(c['write_facing']),int(c['write_pitch'])]
    return c['name']+' '+' '.join(map(str,v))+'\n'
def cases():
    rng=random.Random(12340987)
    for i in range(4096):
        flags=(i&255)|([0,0x1000,0x400000,0x800000][i//256%4])
        angle=rng.uniform(-math.pi,math.pi);pitch=rng.uniform(-math.pi/2,math.pi/2)
        direction=vector([math.cos(angle)*math.cos(pitch),math.sin(angle)*math.cos(pitch),math.sin(pitch)])
        yield dict(name=f'plot_{i}',elapsed=rng.choice([0,1,7,16,100,250,500,1000,60000,0x7fffffff,0xffffffff]),
            flags=flags,flags2=(i//1024)*8,speed=f32(rng.uniform(.1,50)),turn_rate=f32(rng.uniform(.1,6)),pitch_rate=f32(rng.uniform(.1,6)),
            anchor_facing=f32(angle),anchor_pitch=f32(pitch),direction=direction,direction2d=vector([math.cos(angle),math.sin(angle)]),
            pitch_cos=f32(math.cos(pitch)),pitch_sin=f32(math.sin(pitch)),move=[17,23,-11],facing=f32(.123),pitch=f32(-.456),
            write_facing=bool(i%3),write_pitch=bool(i%5))
def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True);src.add_argument('--binary');src.add_argument('--reference')
    dst=p.add_mutually_exclusive_group(required=True);dst.add_argument('--probe');dst.add_argument('--actual')
    p.add_argument('--cpu-reference');p.add_argument('--write-reference');p.add_argument('--failures');a=p.parse_args()
    if a.reference:r=json.loads(Path(a.reference).read_text())['cases']
    else:
        from original_movement_plot_reference import OriginalMovementPlot
        from original_collision_reference import REFERENCE_SHA256
        o=OriginalMovementPlot(a.binary,a.cpu_reference);r=[dict(input=c,output=o.plot(c)) for c in cases()]
        if a.write_reference:
            h=json.dumps(dict(binary_sha256=REFERENCE_SHA256,routine='PlotUnitMovement 0x987b50',fpu_control=0x27f),indent=2)
            Path(a.write_reference).write_text(h[:-2]+',\n  "cases": [\n'+',\n'.join('    '+json.dumps(v) for v in r)+'\n  ]\n}\n')
    if a.actual:text=Path(a.actual).read_text()
    else:
        result=subprocess.run([a.probe],input=''.join(probe_input(v['input']) for v in r),capture_output=True,text=True,timeout=90)
        if result.returncode:raise RuntimeError(result.stderr)
        text=result.stdout
    actual=[json.loads(line) for line in text.splitlines()]
    if len(r)!=len(actual):raise RuntimeError('Missing movement projection results')
    failures=[dict(input=v['input'],expected=v['output'],actual=g) for v,g in zip(r,actual) if not matches(v['output'],g)]
    if a.failures:Path(a.failures).write_text(json.dumps(failures,indent=2))
    print(json.dumps(dict(cases=len(r),failures=len(failures),details=failures[:2]),indent=2))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
