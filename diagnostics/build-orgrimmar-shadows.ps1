param([switch]$LinkOnly)
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
$root = 'D:\games\WoWee'
Set-Location "$root\build"
New-Item -ItemType Directory -Force -Path 'orgrimmar-shadow-objects','orgrimmar-shadow-shaders' | Out-Null
$commands = Get-Content compile_commands.json -Raw | ConvertFrom-Json
if (-not $LinkOnly) {
foreach ($file in @('src/core/application.cpp','src/rendering/wmo_renderer.cpp')) {
    $entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/$file" -and $_.output -match '/wowee\.dir/' } | Select-Object -First 1
    if (-not $entry) { throw "Missing command: $file" }
    $object = 'orgrimmar-shadow-objects/' + $file + '.obj'
    New-Item -ItemType Directory -Force -Path (Split-Path $object) | Out-Null
    $command = [regex]::Replace($entry.command, '-o \S+', "-o $object")
    & cmd.exe '/d' '/s' '/c' $command
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $file" }
    Write-Output "Compiled $file"
}
& 'C:\msys64\mingw64\bin\cmake.exe' --build . --target test_wmo_material_class --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'WMO test build failed' }
& "$root\build\bin\test_wmo_material_class.exe"
if ($LASTEXITCODE -ne 0) { throw 'WMO test failed' }
}
& 'C:\msys64\mingw64\bin\glslc.exe' '-fshader-stage=fragment' '-O' "$root\assets\shaders\wmo.frag.glsl" '-o' 'orgrimmar-shadow-shaders/wmo.frag.spv'
if ($LASTEXITCODE -ne 0) { throw 'WMO shader compile failed' }
& 'C:\msys64\mingw64\bin\spirv-val.exe' 'orgrimmar-shadow-shaders/wmo.frag.spv'
if ($LASTEXITCODE -ne 0) { throw 'SPIR-V validation failed' }
$baseline = Get-Content orgrimmar-light-fixed.rsp
$lines = foreach ($line in $baseline) {
    if ($line -eq '"orgrimmar-light-v2-objects/src/core/application.cpp.obj"') { '"orgrimmar-shadow-objects/src/core/application.cpp.obj"' }
    elseif ($line -eq '"fast-session-link/src/rendering/wmo_renderer.cpp.obj"') { '"orgrimmar-shadow-objects/src/rendering/wmo_renderer.cpp.obj"' }
    else { $line }
}
[System.IO.File]::WriteAllText("$root\build\orgrimmar-shadow-client.rsp", ($lines -join "`n") + "`n")
& 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '-flto=4' '-fno-fat-lto-objects' '-Wno-odr' '@orgrimmar-shadow-client.rsp' '-o' 'bin/wowee-orgrimmar-shadows.exe' '-Wl,--major-image-version,0,--minor-image-version,0'
if ($LASTEXITCODE -ne 0) { throw 'Client link failed' }
Write-Output 'Linked Orgrimmar receiver-shadow fix'
