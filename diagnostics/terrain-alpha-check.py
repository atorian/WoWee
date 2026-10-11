"""Compare the old RLE rule to exact MCAL runs on an extracted Northrend tile."""
from pathlib import Path
import struct, json
from PIL import Image, ImageDraw
root = Path('D:/games/WoWee')
b = (root/'Data/expansions/wotlk/world/maps/northrend/northrend_31_21.adt').read_bytes()
o=0; chunks=[]; textures=[]
while o+8 <= len(b):
    tag=b[o:o+4][::-1]; n=struct.unpack_from('<I',b,o+4)[0]
    d=b[o+8:o+8+n]; o+=n+8
    if tag==b'MTEX': textures=d.decode().split('\0')[:-1]
    if tag==b'MCNK': chunks.append(d)

def decode(blob, start, end, old=False):
    out=[]; i=start; commands=0
    while len(out)<4096 and i<end:
        cmd=blob[i]; i+=1; count=(cmd&127)+(1 if old else 0); commands+=1
        if count==0: continue
        if cmd&128:
            if i>=end: break
            out += [blob[i]]*min(count,4096-len(out)); i+=1
        else:
            count=min(count,4096-len(out),end-i)
            out.extend(blob[i:i+count]); i+=count
    return bytes((out+[0]*4096)[:4096]),len(out),i-start

before=Image.new('RGB',(1024,1024)); after=Image.new('RGB',(1024,1024))
changed=total=maps=0; malformed=0; reference=bytearray()
for d in chunks:
    ix,iy=struct.unpack_from('<2I',d,4); n=struct.unpack_from('<I',d,12)[0]
    ls,als=struct.unpack_from('<I',d,28)[0],struct.unpack_from('<I',d,36)[0]
    size=struct.unpack_from('<I',d,40)[0]-8; blob=d[als:als+size]
    layers=[struct.unpack_from('<4I',d,ls+i*16) for i in range(n)]
    oldmaps=[]; newmaps=[]
    for j,l in enumerate(layers[1:],1):
        end=layers[j+1][2] if j+1<n else len(blob)
        if l[1]&512:
            oldmap,_,_=decode(blob,l[2],len(blob),True)
            newmap,amount,used=decode(blob,l[2],end)
            malformed += amount!=4096 or used!=end-l[2]
            changed+=sum(a!=bb for a,bb in zip(oldmap,newmap));total+=4096;maps+=1
            reference.extend(struct.pack('<II',iy*16+ix,j)+newmap)
        else:
            newmap=blob[l[2]:l[2]+4096];oldmap=newmap
        oldmaps.append(oldmap);newmaps.append(newmap)
    def colour(tex):
        name=textures[tex].lower()
        if 'rock' in name: return (92,87,99)
        if 'snow' in name: return (224,224,234)
        if 'grass' in name or 'bush' in name: return (138,136,61)
        if 'root' in name or 'dirt' in name: return (102,74,43)
        return (120,114,107)
    colors=[colour(l[0]) for l in layers]
    oldpix=[];newpix=[]
    for k in range(4096):
        c=list(colors[0]);w=[am[k]/255 for am in newmaps];sumw=sum(w)
        for j,am in enumerate(oldmaps):
            a=am[k]/255;c=[cc*(1-a)+col*a for cc,col in zip(c,colors[j+1])]
        oldpix.append(tuple(round(x) for x in c))
        newpix.append(tuple(round(colors[0][cc]*max(1-sumw,0)+sum(colors[j+1][cc]*w[j] for j in range(len(w)))/max(sumw,1)) for cc in range(3)))
    oldim=Image.new('RGB',(64,64));oldim.putdata(oldpix);before.paste(oldim,(ix*64,iy*64))
    newim=Image.new('RGB',(64,64));newim.putdata(newpix);after.paste(newim,(ix*64,iy*64))
canvas=Image.new('RGB',(1280,675),(25,25,25));draw=ImageDraw.Draw(canvas)
draw.text((12,10),'BEFORE: +1 RLE counts, sequential blend',fill='white')
draw.text((652,10),'AFTER: exact counts, absolute coverage',fill='white')
canvas.paste(before.resize((640,640)),(0,35));canvas.paste(after.resize((640,640)),(640,35))
canvas.save(root/'build/terrain-alpha-before-after.png')
(root/'build/terrain-alpha-reference.bin').write_bytes(reference)
result={'tile':'Northrend_31_21','compressed_layers':maps,'changed_alpha_texels':changed,'total_alpha_texels':total,'changed_percent':round(100*changed/total,2),'incomplete_or_unconsumed_correct_streams':malformed}
(root/'build/terrain-alpha-check.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
tex=root/'Data/expansions/wotlk/tileset/expansion02/stormpeaks/sp_crystalsongrockc.blp'
try:
    Image.open(tex).save(root/'build/crystalsong-rock-source.png')
except Exception as ex: print('Texture preview:',ex)
