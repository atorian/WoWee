#!/usr/bin/env python3
"""Differential contact tests against the original 12340 machine code.

No network or game process is involved. --write-reference records original
results for regression tests on machines without the proprietary executable.
"""
import argparse
import json
import math
from pathlib import Path
import random
import struct
import subprocess


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def vector(v):
    return [f32(x) for x in v]


def triangle(a, b, c):
    a,b,c=map(vector,(a,b,c))
    u=[b[i]-a[i] for i in range(3)]
    v=[c[i]-a[i] for i in range(3)]
    n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
    length=math.sqrt(sum(x*x for x in n))
    n=vector([x/length for x in n])
    return {'vertices':[a,b,c],'plane':n+[f32(-sum(n[i]*a[i] for i in range(3)))]}


def quad(a,b,c,d):
    return [triangle(a,b,c),triangle(a,c,d)]


def cases():
    wall=quad([0,-5,-5],[0,-5,5],[0,5,5],[0,5,-5])
    floor=quad([-5,-5,0],[5,-5,0],[5,5,0],[-5,5,0])
    ceiling=quad([-5,-5,2.2],[-5,5,2.2],[5,5,2.2],[5,-5,2.2])
    def case(name,position,direction,distance,triangles,radius=1/3,height=2.0277777):
        return dict(name=name,position=vector(position),direction=vector(direction),
                    distance=f32(distance),radius=f32(radius),height=f32(height),triangles=triangles)
    yield case('empty',[0,0,0],[1,0,0],1,[])
    yield case('wall',[-1,0,0],[1,0,0],2,wall)
    yield case('floor',[0,0,1],[0,0,-1],2,floor)
    yield case('ceiling',[0,0,0],[0,0,1],1,ceiling)
    for d in [0,1e-7,9e-7,1e-6,0.001,0.002,0.005,0.01,0.027,0.028,0.1,1,20]:
        yield case(f'small_{d}',[-1/3-0.002,0,0],[1,0,0],d,wall)
    for overlap in [-0.05,-0.028,-0.027,-0.002,-0.001,0,0.001,0.002]:
        for direction in [-1,1]:
            yield case(f'overlap_{overlap}_{direction}',[-1/3-overlap,0,0],[direction,0,0],0.1,wall)
    for angle in [0,30,49,49.999,50,50.001,60,80]:
        rise=math.tan(math.radians(angle))
        ramp=quad([-5,-5,-5*rise],[5,-5,5*rise],[5,5,5*rise],[-5,5,-5*rise])
        yield case(f'ramp_{angle}',[-1,0,1],[1,0,0],3,ramp)
        yield case(f'land_{angle}',[0,0,1],[0,0,-1],3,ramp)
    for radius,height in [(0.2,1),(1/3,2.0277777),(0.5,2),(1,4)]:
        yield case(f'body_{radius}',[-2,0,0],[1,0,0],5,wall,radius,height)
    rng=random.Random(12340)
    for i in range(250):
        angle=rng.uniform(-math.pi,math.pi)
        n=[math.cos(angle),math.sin(angle),0]
        tangent=[-n[1],n[0],0]
        # A plane with inward-facing normal n and bounded height/width.
        origin=[rng.uniform(-20,20),rng.uniform(-20,20),rng.uniform(-2,2)]
        def p(u,z):return [origin[0]+u*tangent[0],origin[1]+u*tangent[1],origin[2]+z]
        triangles=quad(p(-5,-5),p(5,-5),p(5,5),p(-5,5))
        pos=[origin[j]+n[j]*rng.uniform(0.4,2) for j in range(3)]
        pos[2]=origin[2]+rng.uniform(-4,4)
        yield case(f'wall_random_{i}',pos,[-n[0],-n[1],0],3,triangles)
    for origin in [1000,8000,-10000]:
        shift=[origin,origin,origin]
        tris=[triangle(*[[v[j]+shift[j] for j in range(3)] for v in t['vertices']]) for t in wall]
        yield case(f'continent_{origin}',[origin-1,origin,origin],[1,0,0],2,tris)
    # Oblique movement, edges, corners and ceilings must exercise more than
    # axis-aligned walls. Keep the PRNG seed fixed for reproducible failures.
    for i in range(1000):
        position=[rng.uniform(-3,3) for _ in range(3)]
        direction=[rng.uniform(-1,1) for _ in range(3)]
        length=math.sqrt(sum(v*v for v in direction))
        direction=[v/length for v in direction]
        if i%3==0:
            triangles=wall+floor+ceiling
        else:
            triangles=[triangle(*[[rng.uniform(-5,5) for _ in range(3)] for _ in range(3)])
                       for _ in range(4)]
        yield case(f'oblique_{i}',position,direction,rng.uniform(0.001,8),triangles,
                   rng.uniform(0.1,0.8),rng.uniform(1.5,4))


def probe_input(case):
    values=[case['radius'],case['height'],*case['position'],*case['direction'],case['distance'],len(case['triangles'])]
    for tri in case['triangles']:
        values.extend(tri['plane'])
        for v in tri['vertices']:values.extend(v)
    return case['name']+' '+' '.join(map(str,values))+'\n'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary')
    parser.add_argument('--probe',required=True)
    parser.add_argument('--reference')
    parser.add_argument('--write-reference')
    parser.add_argument('--fpu-control',type=lambda v:int(v,0),default=0x27f)
    args=parser.parse_args()
    if args.reference:
        records=json.loads(Path(args.reference).read_text())['cases']
        inputs=[r['input'] for r in records]
        expected=[r['output'] for r in records]
    else:
        if not args.binary:parser.error('--binary or --reference is required')
        from original_collision_reference import OriginalCollision, REFERENCE_SHA256
        original=OriginalCollision(args.binary,args.fpu_control)
        inputs=list(cases())
        expected=[original.distance_to_move(c) for c in inputs]
        # Reference harness controls: neither a broken ABI nor an empty scene
        # may turn every query into a successful no-contact comparison.
        assert expected[0]['distance']==1 and not expected[0]['planes']
        assert abs(expected[1]['distance']-2/3)<1e-6 and expected[1]['planes']
        assert abs(expected[2]['distance']-1)<1e-6 and expected[2]['planes']
        assert 0<expected[3]['distance']<0.2 and expected[3]['planes']
        if args.write_reference:
            header=json.dumps(dict(binary_sha256=REFERENCE_SHA256,
                                   fpu_control=args.fpu_control),indent=2)
            records=',\n'.join('    '+json.dumps(dict(input=i,output=o)) for i,o in zip(inputs,expected))
            Path(args.write_reference).write_text(header[:-2]+',\n  "cases": [\n'+records+'\n  ]\n}\n')
    process=subprocess.run([args.probe],input=''.join(map(probe_input,inputs)),text=True,capture_output=True)
    if process.returncode:
        raise RuntimeError(f'Contact probe failed ({process.returncode}):\n{process.stderr}')
    actual=[json.loads(line) for line in process.stdout.splitlines()]
    if len(actual)!=len(expected):raise RuntimeError('Probe dropped results')
    failures=[]
    maximum=0
    for case,want,got in zip(inputs,expected,actual):
        error=abs(want['distance']-f32(got['distance']))
        maximum=max(maximum,error)
        # The probe prints enough digits to recover each binary32 result.
        # Compare the bits, including signed zero: no positional tolerance may
        # conceal a different contact decision at the next simulation step.
        same=lambda a,b:struct.pack('<f',a)==struct.pack('<f',b)
        passed=(case['name']==got['name'] and same(want['distance'],got['distance'])
                and want['facet']==got['facet'] and len(want['planes'])==len(got['planes']))
        if passed:
            passed=all(same(a,b) for pa,pb in zip(want['planes'],got['planes']) for a,b in zip(pa,pb))
        if not passed:failures.append(dict(name=case['name'],expected=want,actual=got))
    print(json.dumps(dict(cases=len(inputs),failures=len(failures),max_distance_error=maximum,details=failures[:20]),indent=2))
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
