$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
foreach ($name in @('character', 'm2', 'terrain', 'wmo', 'grass', 'water', 'skybox')) {
  & 'C:\msys64\mingw64\bin\glslc.exe' '-fshader-stage=fragment' '-O' "$root\assets\shaders\$name.frag.glsl" '-o' "$root\assets\shaders\$name.frag.spv"
  if ($LASTEXITCODE -ne 0) { throw "Shader compilation failed: $name" }
  & 'C:\msys64\mingw64\bin\spirv-val.exe' "$root\assets\shaders\$name.frag.spv"
  if ($LASTEXITCODE -ne 0) { throw "SPIR-V validation failed: $name" }
}
Write-Output 'Seven atmospheric surface/sky shaders compile and pass SPIR-V validation'
