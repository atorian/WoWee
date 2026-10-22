"""Compare surface_rt_probe dumps to an independent square-shadow reference.

Run rt_probe --surfaces with WOWEE_SURFACE_PROBE_DUMP pointing to an existing
directory before and after a change. This is image accuracy, not a benchmark.
"""
import argparse
import json, math, struct, zlib
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('before',type=Path)
parser.add_argument('after',type=Path)
args=parser.parse_args()
def read(p):
    data=p.read_bytes();header,pixels=data.split(b'\n255\n',1);w,h=map(int,header.splitlines()[1].split());return w,h,list(pixels)
def png(p,w,h,data):
    def chunk(name,value):return struct.pack('>I',len(value))+name+value+struct.pack('>I',zlib.crc32(name+value)&0xffffffff)
    rows=b''.join(b'\0'+bytes(data[y*w:(y+1)*w]) for y in range(h))
    p.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,0,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
w,h,before=read(args.before/'shadow-settled-ms1.pgm')
aw,ah,after=read(args.after/'shadow-settled-ms1.pgm')
if (aw,ah)!=(w,h):raise RuntimeError('Compared images have different extents')
# Independent analytic reference: orthographic wall, square blocker at five
# units from the receiver, uniformly sampled solar disk, exact pixel/rectangle
# overlap. No production reconstruction, history or normal filter is reused.
directions=[]
for i in range(256):
    r=math.sqrt((i+.5)/256)*5*.0087
    angle=i*math.pi*(3-math.sqrt(5))
    directions.append((r*math.cos(angle),r*math.sin(angle)))
reference=[]
for y in range(h):
    loz=y*16/h-8;hiz=(y+1)*16/h-8
    for x in range(w):
        loy=x*16/w-8;hiy=(x+1)*16/w-8
        shade=0
        for dy,dz in directions:
            shade+=max(0,min(hiy,2+dy)-max(loy,-2+dy))*max(0,min(hiz,2+dz)-max(loz,-2+dz))/((16/w)*(16/h))
        reference.append(round(255*(1-shade/len(directions))))
out=args.after
png(out/'reference.png',w,h,reference)
# Magnify the same pixel data without smoothing to make the lattice visible.
scale=6;joined=[]
left,right,top,bottom=w//4,3*w//4,h//4,3*h//4
for y in range(top,bottom):
    row=[]
    for data in (before,after,reference):
        row.extend(v for x in range(left,right) for v in [data[y*w+x]]*scale)
    for _ in range(scale):joined.extend(row)
png(out/'before-after-reference.png',(right-left)*scale*3,(bottom-top)*scale,joined)
report={'reference':'Independent analytic square-caster visibility: 256 sun-disk samples and exact pixel coverage; no production filter. Fixed orthographic scene.',
 'before_mean_absolute_shadow_error':sum(abs(a-b) for a,b in zip(before,reference))/(w*h*255),
 'after_mean_absolute_shadow_error':sum(abs(a-b) for a,b in zip(after,reference))/(w*h*255)}
(out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
