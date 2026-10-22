#!/usr/bin/env python3
"""A frame without a player shadow projection still binds readable empty maps."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('binary',type=Path)
    parser.add_argument('scratch',type=Path)
    args=parser.parse_args()
    binary=args.binary.resolve();out=args.scratch.resolve();out.mkdir(parents=True,exist_ok=True)
    scene=dict(map='Northrend',mapId=571,zoneId=4395,hour=12,origin=[5818,405,650],
               playerPosition=[0,0,0],models=[],streamWorld=False,size=[640,480],frames=6,
               msaa=4,fxaa=False,shadowQuality=0,rayTracedLighting=0,volumetricFog=0,
               camera={'offset':[-12,0,0],'lookAt':[0,0,0]},
               m2Layers=[{'x':0,'rect':[-2,-3,2,3],'rgba':[125,150,110,255]}],
               renderTargetChanges=[{'frame':2,'msaa':1},{'frame':4,'msaa':4}])
    source=out/'scene.json';source.write_text(json.dumps(scene),encoding='utf-8')
    env=os.environ.copy();env.update(WOWEE_MODEL_SURFACES='0',WOWEE_LOG_LEVEL='info',
        WOWEE_VULKAN_VALIDATION='1',WOWEE_RESOURCE_ROOT=str(binary.parent),WOWEE_CONFIG_ROOT=str(out/'config'))
    run=subprocess.run([str(binary),str(source),str(out/'scene.png')],cwd=binary.parent,env=env,
                       capture_output=True,timeout=180)
    log=run.stdout+run.stderr;(out/'scene.log').write_bytes(log)
    checked=[line for line in log.splitlines() if not
        (b'loader_get_json: Failed to open JSON file' in line and b'EOSOverlayVkLayer-' in line)]
    if run.returncode or any(token in line for line in checked for token in
        (b'VUID-',b'SYNC-HAZARD',b'Validation Error:',b'[ERROR]',b'Failed to load texture:',b'Texture not found:')):
        raise RuntimeError(f'Unplaced shadow frame failed; inspect {out/"scene.log"}')
    print('PASS: empty shadow maps are readable before a player projection exists and after target recreation')


if __name__=='__main__':
    main()
