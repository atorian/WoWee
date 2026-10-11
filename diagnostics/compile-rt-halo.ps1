$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$compiler = 'C:\msys64\mingw64\bin\glslc.exe'
$root = 'D:\games\WoWee'
foreach ($name in @('character', 'm2', 'terrain', 'wmo')) {
  & $compiler '-fshader-stage=fragment' '-O' "$root\assets\shaders\$name.frag.glsl" '-o' "$root\build\rt-halo-shaders\$name.frag.spv"
  if ($LASTEXITCODE -ne 0) { throw "Shader compilation failed: $name" }
}
& $compiler '-fshader-stage=compute' '-O' "$root\tools\rt_lookup_probe.comp.glsl" '-o' "$root\build\rt-halo-shaders\rt_lookup_probe.comp.spv"
if ($LASTEXITCODE -ne 0) { throw 'Lookup shader compilation failed' }
Write-Output 'Compiled four surface shaders and the GPU regression shader'
