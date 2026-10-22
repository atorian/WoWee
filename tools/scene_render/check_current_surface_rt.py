#!/usr/bin/env python3
"""Current WMO RT reaches the real lighting consumer on both recording paths."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from check_sharp_stars import rgb_pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('scratch', type=Path)
    parser.add_argument('--msaa', type=int, default=1)
    parser.add_argument('--scene', type=Path)
    parser.add_argument('--rebuild', action='store_true')
    args = parser.parse_args()
    binary, scratch = args.binary.resolve(), args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    scene = dict(map='Northrend', mapId=571, zoneId=4395, hour=12,
                 origin=[5818, 405, 650], streamWorld=False, models=[],
                 camera={'offset': [-8, 0, 0], 'lookAt': [0, 0, 0]},
                 wmoLayers=[{'x': 3, 'rect': [-3,-3,3,3], 'rgba': [120,120,120,255], 'flags': 0},
                            {'x': 1, 'rect': [-1,-1,1,1], 'rgba': [120,120,120,255], 'flags': 0}],
                 shadowQuality=0, volumetricFog=0, bloom=0, fogStrength=0)
    if args.scene:
        scene = json.loads(args.scene.read_text(encoding='utf-8-sig'))
    scene.update(size=[640,480], frames=30, msaa=args.msaa, fxaa=False)
    if args.rebuild:
        scene['renderTargetChanges']=[{'frame':2,'msaa':4,'fxaa':True},
                                      {'frame':4,'msaa':1,'fxaa':False},
                                      {'frame':6,'msaa':args.msaa,'fxaa':True}]
    env = os.environ.copy()
    env.update(WOWEE_WMO_SURFACES='1', WOWEE_RENDER_THREAD='1', WOWEE_LOG_LEVEL='info',
               WOWEE_CONFIG_ROOT=str(scratch/'config'), WOWEE_RESOURCE_ROOT=str(binary.parent))
    env.pop('WOWEE_RT_FORCE_SOFTWARE', None)
    images = {}
    for name, mode, serial in [('off',0,False), ('current',3,False), ('serial',3,True)]:
        scene['rayTracedLighting'] = mode
        source=scratch/(name+'.json'); source.write_text(json.dumps(scene),encoding='utf-8')
        env.pop('WOWEE_SINGLE_THREAD_RECORD',None)
        if serial: env['WOWEE_SINGLE_THREAD_RECORD']='1'
        result=subprocess.run([str(binary),str(source),str(scratch/(name+'.png'))],
                              cwd=binary.parent,env=env,capture_output=True,timeout=180)
        log=result.stdout+result.stderr;(scratch/(name+'.log')).write_bytes(log)
        lines=[line for line in log.splitlines() if not
               (b'loader_get_json: Failed to open JSON file' in line and
                line.endswith((b'Overlay\\EOSOverlayVkLayer-Win32.json',b'Overlay\\EOSOverlayVkLayer-Win64.json')))]
        if result.returncode or any(token in line for line in lines for token in
                                   [b'VUID-',b'SYNC-HAZARD',b'Validation Error:',b'[ERROR]']):
            raise RuntimeError(f'{name}: rendering failed; inspect {scratch/(name+".log")}')
        if mode and b'Current surface RT:' not in log:
            raise RuntimeError('current receivers were not traced')
        images[name]=rgb_pixels(scratch/(name+'.png'))
    if images['current'] != images['serial']:
        raise RuntimeError('worker recording changed current RT lighting')
    difference=sum(abs(a-b) for a,b in zip(images['off'],images['current']))/len(images['current'])
    if difference<0.01:
        raise RuntimeError('current RT did not reach the material lighting consumer')
    report={'msaa':args.msaa,'rebuild':args.rebuild,'serial_parallel_changed_channels':0,'rt_mean_channel_change':difference}
    (scratch/'comparison.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))


if __name__=='__main__':
    main()
