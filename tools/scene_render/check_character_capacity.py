#!/usr/bin/env python3
"""All character instances remain visible after a dense material workload."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from check_sharp_stars import rgb_pixels

p=argparse.ArgumentParser();p.add_argument('binary',type=Path);p.add_argument('scratch',type=Path)
a=p.parse_args();binary=a.binary.resolve();out=a.scratch.resolve();out.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update(WOWEE_CONFIG_ROOT=str(out/'config'),WOWEE_RESOURCE_ROOT=str(binary.parent),
    WOWEE_MODEL_SURFACES='0',WOWEE_LOG_LEVEL='info')
scene=dict(map='Northrend',mapId=571,zoneId=4395,hour=12,origin=[5818,405,650],models=[],streamWorld=False,
    size=[256,256],frames=3,msaa=1,fxaa=False,shadowQuality=0,volumetricFog=0,bloom=0,fogStrength=0,
    rayTracedLighting=0,camera={'offset':[-12,0,0],'lookAt':[0,0,0]})
foreground={'x':1,'rect':[-.16,-.16,.16,.16],'rgba':[40,190,40,255],'flags':4}
images={}
for name in ('reference','dense'):
    scene['characterLayers']=[foreground]*(64 if name=='dense' else 1)
    scene['characterLayerInstances']=[{'offset':[0,(i%13-6)*.4,(i//13-2)*.4]} for i in range(65)]
    scene['playerPosition']=scene['origin']
    path=out/(name+'.json');path.write_text(json.dumps(scene),encoding='utf-8')
    run=subprocess.run([str(binary),str(path),str(out/(name+'.png'))],cwd=binary.parent,env=env,capture_output=True,timeout=180)
    log=run.stdout+run.stderr;(out/(name+'.log')).write_bytes(log)
    if run.returncode or any(t in log for t in [b'VUID-',b'SYNC-HAZARD',b'Validation Error:',b'Failed to load texture:',b'Texture not found:']):
        raise RuntimeError(f'{name}: render/validation failed; inspect log')
    images[name]=rgb_pixels(out/(name+'.png'))
changed=sum(a!=b for a,b in zip(images['reference'],images['dense']))
if changed:raise RuntimeError(f'Character batches disappeared after dense workload: {changed} changed channels')
print('PASS: final character batch survives a dense material workload')
