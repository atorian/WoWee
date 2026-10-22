#!/usr/bin/env python3
"""Real M2 instancing/materials keep their lighting when evaluated once per surface."""
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
    parser.add_argument('--mixed', action='store_true')
    parser.add_argument('--rebuild', action='store_true')
    parser.add_argument('--software', action='store_true')
    parser.add_argument('--regions', action='store_true')
    parser.add_argument('--near-plane', action='store_true')
    args = parser.parse_args()
    binary, out = args.binary.resolve(), args.scratch.resolve()
    out.mkdir(parents=True, exist_ok=True)
    scene = dict(map='Northrend', mapId=571, zoneId=4395, hour=12,
                 origin=[5818,405,650], models=[], streamWorld=False,
                 size=[640,480], frames=30, msaa=args.msaa, fxaa=False,
                 shadowQuality=0, volumetricFog=0, bloom=0, fogStrength=0,
                 camera={'offset':[-12,0,0], 'lookAt':[0,0,0]},
                 m2Layers=[
                     {'x':3, 'rect':[-2,-3,2,3], 'rgba':[125,150,110,255]},
                     {'x':2, 'rect':[-1.8,-2.8,1.8,2.8], 'rgba':[80,130,190,255],
                      'secondRgba':[60,20,10,128], 'shader':0x8002},
                     {'x':1, 'rect':[-1.3,-2.3,1.3,2.3], 'rgba':[160,100,60,255],
                      'cutout':True, 'blend':1},
                     {'x':0, 'rect':[-.7,-1.7,.7,1.7], 'rgba':[50,130,180,80], 'blend':2}],
                 m2LayerInstances=[{'offset':[0,-2.3,0]},
                                   {'offset':[0,2.3,0], 'interiorAmbient':[.12,.25,.35],
                                    'interiorDirect':[.5,.2,.1]}])
    if args.regions:
        # An opaque alpha-key batch stays in the authored forward sequence.
        # Geometry on both sides forces separate surface runs over the same
        # screen area, including old keys where the later layer has holes.
        scene['m2Layers'][2:2] = [
            {'x':1.8, 'rect':[-1.6,-2.6,1.6,2.6], 'rgba':[120,90,60,255], 'blend':1},
            {'x':1.5, 'rect':[-1.4,-2.4,1.4,2.4], 'rgba':[60,160,100,255]}]
        scene['size']=[641,479]  # Partial tiles at the target boundary.
    if args.near_plane:
        scene['camera']={'offset':[1.7,0,0], 'lookAt':[3,0,0]}
        for layer in scene['m2Layers']: layer['flags']=4  # Two sided.
    if args.mixed:
        scene['wmoLayers']=[{'x':4,'rect':[-6,-4,6,4],'rgba':[100,100,120,255],'flags':0},
                            {'x':-1,'rect':[-.5,-4,.5,4],'rgba':[180,80,80,100],'blend':2,'flags':0}]
    if args.rebuild:
        scene['renderTargetChanges']=[{'frame':2,'msaa':4,'fxaa':True},
                                      {'frame':4,'msaa':1,'fxaa':False},
                                      {'frame':6,'msaa':args.msaa,'fxaa':True}]
    env=os.environ.copy()
    env.update(WOWEE_RENDER_THREAD='1',WOWEE_LOG_LEVEL='info',WOWEE_CONFIG_ROOT=str(out/'config'),
               WOWEE_RESOURCE_ROOT=str(binary.parent))
    env.pop('WOWEE_WMO_SURFACES',None)
    env.pop('WOWEE_RT_FORCE_SOFTWARE',None)
    if args.software: env['WOWEE_RT_FORCE_SOFTWARE']='1'
    images={}
    cases=[('forward',False,0,False),('surface',True,0,False),
           ('current',True,3,False),('serial',True,3,True)]
    if args.regions: cases.append(('full',True,3,False))
    for label, surfaces, mode, serial in cases:
        env.pop('WOWEE_SURFACE_FULL_EXTENT',None)
        if label=='full': env['WOWEE_SURFACE_FULL_EXTENT']='1'
        env.pop('WOWEE_MODEL_SURFACES',None)
        env.pop('WOWEE_SINGLE_THREAD_RECORD',None)
        if surfaces: env['WOWEE_MODEL_SURFACES']='1'
        if serial: env['WOWEE_SINGLE_THREAD_RECORD']='1'
        scene['rayTracedLighting']=mode
        source=out/(label+'.json');source.write_text(json.dumps(scene),encoding='utf-8')
        result=subprocess.run([str(binary),str(source),str(out/(label+'.png'))],cwd=binary.parent,
                              env=env,capture_output=True,timeout=180)
        log=result.stdout+result.stderr;(out/(label+'.log')).write_bytes(log)
        checked=[line for line in log.splitlines() if not
                 (b'loader_get_json: Failed to open JSON file ' in line and line.endswith(
                     (b'Overlay\\EOSOverlayVkLayer-Win32.json',b'Overlay\\EOSOverlayVkLayer-Win64.json')))]
        if result.returncode or any(token in line for line in checked for token in
                                   (b'VUID-',b'SYNC-HAZARD',b'Validation Error:',b'[ERROR]')):
            raise RuntimeError(f'{label}: render/validation failure; inspect {out/(label+".log")}')
        images[label]=rgb_pixels(out/(label+'.png'))
    # Surface storage quantizes color/normals. It must not lose a texture stage,
    # instance light, alpha silhouette or the authored transparent layer.
    delta=[abs(a-b) for a,b in zip(images['forward'],images['surface'])]
    mean=sum(delta)/len(delta)
    substantial=sum(v>8 for v in delta)/len(delta)
    if mean>.25 or substantial>.002:
        raise RuntimeError(f'evaluated materials changed the image: mean={mean}, fraction>8={substantial}')
    if images['current']!=images['serial']:
        raise RuntimeError('parallel recording changed M2 lighting')
    rt=sum(abs(a-b) for a,b in zip(images['surface'],images['current']))/len(delta)
    if args.regions and images['current']!=images['full']:
        changed=sum(a!=b for a,b in zip(images['current'],images['full']))
        raise RuntimeError(f'bounded surface runs changed {changed} image channels')
    if rt<.01 and not args.near_plane: raise RuntimeError('current RT did not affect M2 material lighting')
    report={'msaa':args.msaa,'mixed':args.mixed,'software':args.software,'rebuild':args.rebuild,
            'regions':args.regions,'near_plane':args.near_plane,
            'bounded_full_changed_channels':0 if args.regions else None,
            'forward_surface_mean_channel_error':mean,'fraction_error_over_8':substantial,
            'rt_mean_channel_change':rt,'serial_parallel_changed_channels':0}
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))


if __name__=='__main__':
    main()
