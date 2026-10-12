$ErrorActionPreference='Stop'
$env:Path='C:\msys64\mingw64\bin;'+$env:Path
& 'C:\msys64\mingw64\bin\ffmpeg.exe' '-y' '-i' 'D:\games\WoWee\build\glass-lamps-scene-window-2-3.mp4' '-vf' 'select=eq(n\,10)' '-frames:v' '1' 'D:\games\WoWee\build\glass-lamps-scene-window.png'
if ($LASTEXITCODE -ne 0) { throw 'Frame extraction failed' }
