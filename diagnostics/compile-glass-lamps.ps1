$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
foreach ($name in @('character.frag','terrain.frag','wmo.frag','m2.frag','rt_trace.comp')) {
    $stage = if ($name.EndsWith('.comp')) { '-fshader-stage=compute' } else { '-fshader-stage=fragment' }
    & 'C:\msys64\mingw64\bin\glslc.exe' $stage '-O' "$root\assets\shaders\$name.glsl" '-o' "$root\build\glass-lamps-shaders\$name.spv"
    if ($LASTEXITCODE -ne 0) { throw "Shader failed: $name" }
}
& 'C:\msys64\mingw64\bin\glslc.exe' '-fshader-stage=compute' '-O' '-DRT_HARDWARE' '--target-env=vulkan1.2' "$root\assets\shaders\rt_trace.comp.glsl" '-o' "$root\build\glass-lamps-shaders\rt_trace_hw.comp.spv"
if ($LASTEXITCODE -ne 0) { throw 'Hardware reflection shader failed' }
& 'C:\msys64\mingw64\bin\glslc.exe' '-fshader-stage=compute' '-O' "$root\tools\rt_lookup_probe.comp.glsl" '-o' "$root\build\glass-lamps-shaders\rt_lookup_probe.comp.spv"
if ($LASTEXITCODE -ne 0) { throw 'Lookup regression shader failed' }
Write-Output 'All reflection and receiver shaders compiled'
