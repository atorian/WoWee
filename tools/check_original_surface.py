#!/usr/bin/env python3
"""Compare the surface phase with original build 12340 execution, offline."""
import argparse
import json
import math
from pathlib import Path
import random
import struct
import subprocess
from check_original_collision import quad, triangle, f32, vector

def cases():
 floor=quad([-20,-20,0],[20,-20,0],[20,20,0],[-20,20,0])
 wall=quad([0,-10,0],[0,-10,5],[0,10,5],[0,10,0])
 def case(name,position,direction,distance,triangles,**kw):
  return dict(name=name,position=vector(position),direction=vector(direction),distance=f32(distance),radius=f32(1/3),height=f32(2.0277777),step_height=1,elapsed=16,flags=1,step_start=f32(position[2]),facing=f32(math.atan2(direction[1],direction[0])),triangles=triangles,**kw)
 yield case('empty',[0,0,0],[1,0],.1,[])
 for d in [0,1e-7,1e-6,.001,.005,.01,.1,.3,1,3]:
  yield case(f'floor_{d}',[-.5,0,0],[1,0],d,floor)
  yield case(f'wall_{d}',[-.5,0,0],[1,0],d,floor+wall)
 for h in [.1,.3,.5,.9,1,1.001,1.1,2]:
  step=quad([0,-10,0],[0,-10,h],[0,10,h],[0,10,0])+quad([0,-10,h],[10,-10,h],[10,10,h],[0,10,h])
  for d in [.005,.05,.3,.7,1.5]:
   yield case(f'step_{h}_{d}',[-.5,0,0],[1,0],d,floor+step)
  for ceiling_height in [2.03,2.1,2.5,3]:
   ceiling=quad([-10,-10,ceiling_height],[-10,10,ceiling_height],[10,10,ceiling_height],[10,-10,ceiling_height])
   yield case(f'headroom_{h}_{ceiling_height}',[-.5,0,0],[1,0],.3,floor+step+ceiling)
 for angle in [0,10,30,49,49.999,50,50.001,60,80]:
  rise=math.tan(math.radians(angle))
  ramp=quad([-10,-10,-10*rise],[10,-10,10*rise],[10,10,10*rise],[-10,10,-10*rise])
  for sign in [-1,1]:
   for d in [.005,.1,.3]:yield case(f'ramp_{angle}_{sign}_{d}',[0,0,0],[sign,0],d,ramp)
 ledge=quad([-10,-10,0],[0,-10,0],[0,10,0],[-10,10,0])
 for x in [-.5,-.1,0,.1,.5]:
  yield case(f'ledge_{x}',[x,0,0],[1,0],.3,ledge)
 for angle in [0,.3,.7]:
  for width in [.05,.3,.7,1,2]:
   for h in [.1,.5,.99,1.01]:
    step=quad([0,-width/2,0],[0,-width/2,h],[0,width/2,h],[0,width/2,0])+quad([0,-width/2,h],[2,-width/2,h],[2,width/2,h],[0,width/2,h])
    yield case(f'narrow_{angle}_{width}_{h}',[-.5,0,0],[math.cos(angle),math.sin(angle)],.7,floor+step)
 for angle in [20,49,50,60,80]:
  rise=math.tan(math.radians(angle))
  ramp=quad([0,-5,0],[2,-5,2*rise],[2,5,2*rise],[0,5,0])
  for y in [-5,-4.99,-4.9,0,4.99,5]:
   yield case(f'ramp_edge_{angle}_{y}',[-.1,y,0],[1,0],.7,floor+ramp)
 rng=random.Random(12340)
 for i in range(100):
  angle=rng.uniform(-1,1);d=rng.uniform(.005,1)
  yield case(f'wall_slide_{i}',[-.5,rng.uniform(-1,1),0],[math.cos(angle),math.sin(angle)],d,floor+wall)

 for i in range(300):
  h=rng.uniform(.01,1.2);a=rng.uniform(-1,1);d=rng.uniform(.005,2)
  step=quad([0,-3,0],[0,-3,h],[0,3,h],[0,3,0])+quad([0,-3,h],[3,-3,h],[3,3,h],[0,3,h])
  c=case(f'step_random_{i}',[-rng.uniform(.001,.7),rng.uniform(-3.4,3.4),0],[math.cos(a),math.sin(a)],d,floor+step)
  if i%4==0:
   c['flags']|=0x4000000;c['position'][2]=f32(rng.uniform(.01,.8))
  if i%5==0:
   c['radius']=f32(rng.uniform(.1,.6));c['height']=f32(rng.uniform(1,3));c['step_height']=f32(rng.uniform(.5,2))
  yield c

 for i in range(300):
  h=rng.uniform(.02,1.2);angle=rng.uniform(-math.pi,math.pi);d=rng.uniform(.01,1.5)
  ox,oy=(rng.uniform(-10000,10000),rng.uniform(-10000,10000)) if i%2 else (0,0)
  oz=rng.uniform(-100,100) if i%2 else 0
  co,si=math.cos(angle),math.sin(angle)
  def transform(v):return [ox+v[0]*co-v[1]*si,oy+v[0]*si+v[1]*co,oz+v[2]]
  step=quad([0,-3,0],[0,-3,h],[0,3,h],[0,3,0])+quad([0,-3,h],[3,-3,h],[3,3,h],[0,3,h])
  tris=[triangle(*[transform(v) for v in t['vertices']]) for t in floor+step]
  yield case(f'transformed_{i}',transform([-.5,rng.uniform(-3.4,3.4),0]),[co,si],d,tris)

 for owner in [0, 0xF110000000001234, 0xFFFFFFFFFFFFFFFF]:
  owned=[dict(t,owner=owner) for t in floor]
  yield case(f'owner_{owner}',[0,0,0],[1,0],.1,owned)
 for angle in [49,60,79,80,81]:
  rise=math.tan(math.radians(angle))
  ramp=quad([-10,-10,-10*rise],[10,-10,10*rise],[10,10,10*rise],[-10,10,-10*rise])
  yield case(f'uncontrolled_ramp_{angle}',[0,0,0],[1,0],.1,ramp,client_controlled=False)


def probe_input(c):
    values = [c['radius'], c['height'], c.get('step_height', 1), int(c.get('client_controlled', True)), *c['position'],
              *c['direction'][:2], c['distance'], c.get('elapsed', 16), c.get('flags', 1),
              c.get('step_start', c['position'][2]), c.get('facing', 0), c.get('pitch', 0),
              c.get('flags2', 0), c.get('anchor_elapsed', 0), c.get('anchor_facing', 0),
              c.get('anchor_pitch', 0), c.get('fall_time', 0), c.get('fall_start', 0),
              c.get('fall_velocity', 0), c.get('current_speed', 0),
              *c.get('anchor_position', [0, 0, 0]), *c.get('move_direction', [0, 0, 0]),
              *c.get('move_direction_2d', [0, 0]), len(c['triangles'])]
    for t in c['triangles']:
        values.extend(t['plane'])
        values.append(t.get('owner', 0))
        for v in t['vertices']:
            values.extend(v)
    return c['name'] + ' ' + ' '.join(map(str, values)) + '\n'


def matches(want, got):
    if isinstance(want, float):
        return struct.pack('<f', want) == struct.pack('<f', got)
    if isinstance(want, list):
        return isinstance(got, list) and len(want) == len(got) and all(map(matches, want, got))
    if isinstance(want, dict):
        return want.keys() == got.keys() and all(matches(want[k], got[k]) for k in want)
    return want == got


def trajectory_cases(original):
    # Feed each original output back as the next input. Comparing every step
    # then proves propagation, rather than hiding errors in a final endpoint.
    floor = quad([-20, -10, 0], [20, -10, 0], [20, 10, 0], [-20, 10, 0])
    stairs = []
    for i in range(5):
        x = i * .8
        h = (i + 1) * .2
        stairs += quad([x, -3, 0], [x, -3, h], [x, 3, h], [x, 3, 0])
        stairs += quad([x, -3, h], [x+.8, -3, h], [x+.8, 3, h], [x, 3, h])
    for schedule in [[16], [7, 23, 11, 31], [100]]:
        case = dict(radius=f32(1/3), height=f32(2.0277777), position=[-1, 0, 0],
                    direction=[1, 0], triangles=floor+stairs, flags=1, current_speed=7)
        for frame in range(120):
            elapsed = schedule[frame % len(schedule)]
            case.update(name=f'trajectory_{schedule[0]}_{frame}', elapsed=elapsed,
                        distance=f32(elapsed*.001*7))
            result = original.surface_move(case)
            yield dict(input=dict(case), output=result)
            if result['flags'] & 0x1000:
                break  # The next phase must be Fall, never another surface step.
            case.update({k:v for k,v in result.items() if k not in ('name', 'elapsed', 'transport')})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--binary')
    source.add_argument('--reference')
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument('--probe')
    output.add_argument('--actual', help='JSON lines captured from the probe')
    parser.add_argument('--write-reference')
    parser.add_argument('--fpu-control', type=lambda v:int(v, 0), default=0x27f)
    args = parser.parse_args()
    if args.reference:
        records = json.loads(Path(args.reference).read_text())['cases']
    else:
        from original_collision_reference import OriginalCollision, REFERENCE_SHA256
        original = OriginalCollision(args.binary, args.fpu_control)
        records = [dict(input=c, output=original.surface_move(c)) for c in cases()]
        records.extend(trajectory_cases(original))
        # A missing scene or a broken call ABI must not make the matrix pass.
        assert records[0]['output']['flags'] & 0x1000
        assert records[0]['output']['elapsed'] == 0
        by_name = {r['input']['name']:r['output'] for r in records}
        assert by_name['wall_1']['position'][0] < -.3
        assert by_name['step_0.5_0.7']['position'][2] > .49
        if args.write_reference:
            header = json.dumps(dict(binary_sha256=REFERENCE_SHA256, fpu_control=args.fpu_control,
                                     routine='TraceSurface 0x7620f0'), indent=2)
            rows = ',\n'.join('    '+json.dumps(r) for r in records)
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+rows+'\n  ]\n}\n')
    if args.actual:
        output_text = Path(args.actual).read_text()
    else:
        process = subprocess.run([args.probe], input=''.join(probe_input(r['input']) for r in records),
                                 text=True, capture_output=True, timeout=60)
        if process.returncode:
            raise RuntimeError(f'Surface probe failed ({process.returncode}):\n{process.stderr}')
        output_text = process.stdout
    actual = [json.loads(line) for line in output_text.splitlines()]
    if len(actual) != len(records):
        raise RuntimeError(f'Probe returned {len(actual)} results for {len(records)} inputs')
    failures = [dict(name=r['input']['name'], expected=r['output'], actual=g)
                for r,g in zip(records, actual) if not matches(r['output'], g)]
    print(json.dumps(dict(cases=len(records), failures=len(failures), details=failures[:12]), indent=2))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
