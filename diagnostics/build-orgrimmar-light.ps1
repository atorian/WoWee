$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
$root = 'D:\games\WoWee'
Set-Location "$root\build"
New-Item -ItemType Directory -Force -Path 'orgrimmar-light-v2-objects' | Out-Null
$commands = Get-Content compile_commands.json -Raw | ConvertFrom-Json
foreach ($file in @('src/core/application.cpp','src/rendering/lighting_manager.cpp')) {
    $entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/$file" -and $_.output -match '/wowee\.dir/' } | Select-Object -First 1
    if (-not $entry) { throw "Missing command: $file" }
    $object = 'orgrimmar-light-v2-objects/' + $file + '.obj'
    New-Item -ItemType Directory -Force -Path (Split-Path $object) | Out-Null
    $command = [regex]::Replace($entry.command, '-o \S+', "-o $object")
    & cmd.exe '/d' '/s' '/c' $command
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $file" }
    Write-Output "Compiled $file"
}
& 'C:\msys64\mingw64\bin\cmake.exe' --build . --target test_light_headroom --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Light tests build failed' }
& "$root\build\bin\test_light_headroom.exe"
if ($LASTEXITCODE -ne 0) { throw 'Light tests failed' }
$baseline = Get-Content fast-session-client.rsp
foreach ($variant in @('fixed')) {
    $lines = foreach ($line in $baseline) {
        if ($line -eq '"fast-session-link/src/core/application.cpp.obj"') { '"orgrimmar-light-v2-objects/src/core/application.cpp.obj"' }
        elseif ($variant -eq 'fixed' -and $line -eq '"fast-session-link/src/rendering/lighting_manager.cpp.obj"') { '"orgrimmar-light-v2-objects/src/rendering/lighting_manager.cpp.obj"' }
        else { $line }
    }
    [System.IO.File]::WriteAllText("$root\build\orgrimmar-light-$variant.rsp", ($lines -join "`n") + "`n")
    & 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '-flto=4' '-fno-fat-lto-objects' '-Wno-odr' "@orgrimmar-light-$variant.rsp" '-o' "bin/wowee-orgrimmar-light.exe" '-Wl,--major-image-version,0,--minor-image-version,0'
    if ($LASTEXITCODE -ne 0) { throw "Link failed: $variant" }
    Write-Output "Linked Orgrimmar $variant"
}
