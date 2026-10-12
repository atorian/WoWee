"""Link only from stable copies, avoiding other clients' shared object writes."""
from pathlib import Path
import re, subprocess, os, time, hashlib, json
root = Path('D:/games/WoWee')
build = root / 'build'
ninja = (build / 'fountain-fix.ninja').read_text()
line = next(l for l in ninja.splitlines() if l.startswith('build bin/wowee-fountain-fix.exe:'))
objects = re.findall(r'CMakeFiles/wowee\.dir/[^ ]+\.obj', line)
libs = re.search(r'^  LINK_LIBRARIES = (.+)$', ninja[ninja.index(line):], re.M).group(1)
snapshots = []
manifest = []
for rel in objects:
    source = build / rel
    for attempt in range(60):
        try:
            before = source.stat()
            data = source.read_bytes()
            after = source.stat()
            if len(data) > 128 and data[:2] == b'\x64\x86' and before.st_mtime_ns == after.st_mtime_ns and before.st_size == len(data):
                break
        except OSError:
            pass
        time.sleep(0.5)
    else:
        raise RuntimeError(f'Object did not stabilize: {rel}')
    destination = build / 'fountain-objects' / rel
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    snapshots.append('fountain-objects/' + rel)
    manifest.append({'object': rel, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
(build / 'fountain-object-manifest.json').write_text(json.dumps(manifest, indent=2))
response = build / 'fountain-link.rsp'
response.write_text('\n'.join('"' + x + '"' for x in snapshots) + '\n' + libs + '\n')
env = os.environ.copy()
env['PATHEXT'] = '.COM;.EXE;.BAT;.CMD'
env['PATH'] = 'C:/msys64/mingw64/bin;C:/msys64/usr/bin;' + env['PATH']
print(f'Linking {len(snapshots)} stable object snapshots', flush=True)
subprocess.run(['C:/msys64/mingw64/bin/c++.exe', '-O3', '-DNDEBUG', '-flto=4', '-fno-fat-lto-objects', '-Wno-odr', '@fountain-link.rsp', '-o', 'bin/wowee-fountain-fix.exe', '-Wl,--out-implib,lib/libwowee-fountain.dll.a', '-Wl,--major-image-version,0,--minor-image-version,0'], cwd=build, env=env, check=True)
subprocess.run(['C:/msys64/mingw64/bin/cmake.exe', '-E', 'copy_directory', str(root/'assets'), str(build/'bin/assets')], cwd=build, env=env, check=True)
print('Fountain client linked and runtime assets synchronized', flush=True)
