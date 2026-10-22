#!/usr/bin/env python3
"""A retained M2 keeps its pixels while an offscreen workload arrives and leaves."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from check_sharp_stars import rgb_pixels


def main():
    parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('scratch',type=Path)
    args=parser.parse_args();binary=args.binary.resolve();out=args.scratch.resolve();out.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();env.update(WOWEE_CONFIG_ROOT=str(out/'config'),WOWEE_RESOURCE_ROOT=str(binary.parent),
        WOWEE_MODEL_SURFACES='1',WOWEE_LOG_LEVEL='info',WOWEE_SURFACE_PLAN_CHECK='1')
    scene=dict(map='Northrend',mapId=571,zoneId=4395,hour=12,origin=[5818,405,650],playerPosition=[5818,405,650],
        models=[],streamWorld=False,size=[256,256],msaa=1,fxaa=False,shadowQuality=0,volumetricFog=0,
        bloom=0,fogStrength=0,rayTracedLighting=0,camera={'offset':[-12,0,0],'lookAt':[0,0,0]},
        m2Layers=[{'x':0,'rect':[-2,-3,2,3],'rgba':[40,190,40,255]}]*64)
    for frames in (7,14):
        images={}
        for label in ('reference','streamed'):
            scene['frames']=frames;scene['m2LayerInstances']=[{'offset':[0,0,0]}];scene['m2InstanceChanges']=[]
            if label=='streamed':
                scene['m2LayerInstances'] += [{'offset':[-2,20 if i%2 else -20,0],'hidden':True} for i in range(512)]
                scene['m2InstanceChanges']=[{'frame':frame,'instance':i,'hidden':hidden}
                    for frame,hidden in ((4,False),(8,True)) for i in range(1,513)]
            name=f'{label}-{frames}';source=out/(name+'.json');source.write_text(json.dumps(scene),encoding='utf-8')
            run=subprocess.run([str(binary),str(source),str(out/(name+'.png'))],cwd=binary.parent,env=env,capture_output=True,timeout=180)
            log=run.stdout+run.stderr;(out/(name+'.log')).write_bytes(log)
            checked=[line for line in log.splitlines() if not
                (b'loader_get_json: Failed to open JSON file' in line and b'EOSOverlayVkLayer-' in line)]
            if run.returncode or any(token in line for line in checked for token in
                (b'VUID-',b'SYNC-HAZARD',b'Validation Error:',b'[ERROR]',b'Failed to load texture:',b'Texture not found:')):
                raise RuntimeError(f'{name}: render/validation failed')
            if label=='streamed' and log.count(b'scene_render: applying M2 instance change') != (512 if frames==7 else 1024):
                raise RuntimeError('Streaming fixture events did not execute')
            images[label]=rgb_pixels(out/(name+'.png'))
        changed=sum(a!=b for a,b in zip(images['reference'],images['streamed']))
        if changed:raise RuntimeError(f'Retained M2 changed after streaming at frame {frames}: {changed} channels')
    print('PASS: retained M2 pixels survive arrival and departure of a dense offscreen workload')


if __name__=='__main__':
    main()
