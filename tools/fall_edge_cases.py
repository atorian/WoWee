"""Narrow tilted platforms and nearby walls for original falling comparisons."""
import math,random,struct
from check_original_collision import f32,vector,quad,triangle

def cases():
    rng=random.Random(92759)
    for i in range(3000):
        angle=rng.uniform(-math.pi,math.pi);co,si=math.cos(angle),math.sin(angle)
        width=rng.uniform(.05,1);rise=math.tan(math.radians(rng.uniform(0,85)))
        origin=vector([rng.uniform(-17000,17000),rng.uniform(-17000,17000),rng.uniform(-500,500)]) if i%2 else [0,0,0]
        def transform(v):return vector([origin[0]+co*v[0]-si*v[1],origin[1]+si*v[0]+co*v[1],origin[2]+v[2]])
        geometry=quad([-2,-width,-2*rise],[2,-width,2*rise],[2,width,2*rise],[-2,width,-2*rise])
        if i%3==0:geometry=geometry[:1]
        if i%5==0:geometry+=quad([.2,-2,-3],[.2,-2,3],[.2,2,3],[.2,2,-3])
        geometry=[triangle(*[transform(v) for v in t['vertices']]) for t in geometry]
        elapsed=rng.randrange(1,801);direction=vector([co,si]);position=transform([rng.uniform(-1,1),rng.uniform(-width-.4,width+.4),rng.uniform(.05,3)])
        c=dict(name=f'edge_{i}',position=position,direction=direction,distance=f32(elapsed*.007),radius=f32(1/3),height=f32(2.0277777),elapsed=elapsed,
            flags=0x1001,fall_start=f32(position[2]+rng.uniform(-2,2)),current_speed=7,move_direction=direction+[0],move_direction_2d=direction,
            ground_normal=[.1,.2,.3],facing=f32(angle),triangles=geometry,fall_velocity=f32(rng.choice([0,-7.9555473,5])),fall_time=rng.randrange(0,501),client_controlled=True)
        yield c

def adjacent(v,offset):return struct.unpack('<f',struct.pack('<I',struct.unpack('<I',struct.pack('<f',v))[0]+offset))[0]
def boundary_cases():
 for z in [0,1,-1,100,-100,1000,-1000,16000,-16000]:
  for elapsed in [2,7,16,50,100]:
   t=f32(elapsed*f32(.001));v=f32(f32(.11111111)/t-f32(9.6455526)*t)
   for delta in range(-2,3):
    yield dict(name=f'far_{z}_{elapsed}_{delta}',position=[0,0,z],fall_start=z,fall_velocity=adjacent(v,delta),elapsed=elapsed,flags=0x1001,direction=[1,0],distance=f32(elapsed*.007),radius=f32(1/3),height=f32(2.0277777),current_speed=7,triangles=[])
 for peak in [200,400,800,1200]:
  for elapsed in [7,16,50,100]:
   for delta in range(-2,3):
    v=adjacent(f32(-f32(peak*.001)/f32(.051837362)),delta)
    for z in [0,1000]:
     for style in range(3):
      geometry=quad([0,-20,z-20],[0,-20,z+20],[0,20,z+20],[0,20,z-20])
      if style:geometry+=quad([-20,-20,z+.02*style],[-20,20,z+.02*style],[20,20,z+.02*style],[20,-20,z+.02*style])
      yield dict(name=f'peak_{peak}_{elapsed}_{delta}_{z}_{style}',position=[-.34,0,z],fall_start=f32(z-f32(v*v*f32(.025918681))),fall_velocity=v,fall_time=peak-elapsed,elapsed=elapsed,flags=0x1001,direction=[1,0],distance=f32(elapsed*.007),radius=f32(1/3),height=f32(2.0277777),current_speed=7,triangles=geometry)
