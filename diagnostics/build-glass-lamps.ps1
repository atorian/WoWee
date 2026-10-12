$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
$root = 'D:\games\WoWee'
Set-Location "$root\build"
New-Item -ItemType Directory -Force -Path 'glass-lamps-objects','glass-lamps-shaders' | Out-Null
$commands = Get-Content compile_commands.json -Raw | ConvertFrom-Json
$files = @('src/core/application.cpp','src/rendering/renderer.cpp','src/rendering/rt_lighting.cpp','src/rendering/character_preview.cpp','src/rendering/m2_renderer.cpp','src/rendering/m2_renderer_render.cpp','src/rendering/wmo_renderer.cpp','src/rendering/terrain_renderer.cpp')
$jobs = @()
foreach ($file in $files) {
    $entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/$file" -and $_.output -match '/wowee\.dir/' } | Select-Object -First 1
    if (-not $entry) { throw "No compile command for $file" }
    $object = 'glass-lamps-objects/' + $file + '.obj'
    New-Item -ItemType Directory -Force -Path (Split-Path $object) | Out-Null
    $command = [regex]::Replace($entry.command, '-o \S+', "-o $object")
    $label = [System.IO.Path]::GetFileNameWithoutExtension($file)
    $process = Start-Process cmd.exe -ArgumentList @('/d','/s','/c',"`"$command`"") -PassThru -WindowStyle Hidden -RedirectStandardOutput "glass-lamps-$label-build.out" -RedirectStandardError "glass-lamps-$label-build.err"
    $null = $process.Handle
    $jobs += @{Process=$process; Label=$label}
}
$entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/src/main.cpp" } | Select-Object -First 1
$probeCommand = [regex]::Replace($entry.command, '-o \S+', '-o glass-lamps-objects/rt_probe.cpp.obj')
$probeCommand = [regex]::Replace($probeCommand, '-c \S+', "-c $root\tools\rt_probe.cpp")
$process = Start-Process cmd.exe -ArgumentList @('/d','/s','/c',"`"$probeCommand`"") -PassThru -WindowStyle Hidden -RedirectStandardOutput 'glass-lamps-probe-build.out' -RedirectStandardError 'glass-lamps-probe-build.err'
$null = $process.Handle
    $jobs += @{Process=$process; Label='probe'}
foreach ($job in $jobs) {
    $job.Process.WaitForExit()
    $job.Process.Refresh()
    if ($job.Process.ExitCode -ne 0) { Get-Content "glass-lamps-$($job.Label)-build.err"; throw "Compilation failed: $($job.Label)" }
    Write-Output "Compiled $($job.Label)"
}
$entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/tests/test_m2_material.cpp" } | Select-Object -First 1
$command = [regex]::Replace($entry.command, '-o \S+', '-o glass-lamps-objects/test_m2_material.cpp.obj')
& cmd.exe '/d' '/s' '/c' $command
if ($LASTEXITCODE -ne 0) { throw 'Material test compile failed' }
$entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/src/pipeline/m2_loader.cpp" -and $_.output -match '/test_m2_material\.dir/' } | Select-Object -First 1
$command = [regex]::Replace($entry.command, '-o \S+', '-o glass-lamps-objects/test_m2_loader.cpp.obj')
& cmd.exe '/d' '/s' '/c' $command
if ($LASTEXITCODE -ne 0) { throw 'Test M2 loader compile failed' }
$entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/src/core/logger.cpp" -and $_.output -match '/wowee_test_common\.dir/' } | Select-Object -First 1
$command = [regex]::Replace($entry.command, '-o \S+', '-o glass-lamps-objects/test_logger.cpp.obj')
& cmd.exe '/d' '/s' '/c' $command
if ($LASTEXITCODE -ne 0) { throw 'Test logger compile failed' }
$ninja = Get-Content build.ninja -Raw
$line = @($ninja -split "`n" | Where-Object { $_.StartsWith('build bin/test_m2_material.exe:') })[0]
$objects = [regex]::Matches($line, 'tests/CMakeFiles/test_m2_material\.dir/[^ ]+\.obj') | ForEach-Object { $_.Value }
$objects = $objects | ForEach-Object { if ($_ -match '/test_m2_material.cpp.obj$') { 'glass-lamps-objects/test_m2_material.cpp.obj' } else { 'glass-lamps-objects/test_m2_loader.cpp.obj' } }
$block = $ninja.Substring($ninja.IndexOf($line))
$objects += 'glass-lamps-objects/test_logger.cpp.obj'
$libs = [regex]::Match($block, '(?m)^  LINK_LIBRARIES = (.+)$').Groups[1].Value.Trim()
[System.IO.File]::WriteAllText("$root\build\glass-lamps-material.rsp", ($objects -join "`n") + "`n" + $libs + "`n")
& 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '@glass-lamps-material.rsp' '-o' 'bin/test_glass_lamps_material.exe'
if ($LASTEXITCODE -ne 0) { throw 'Material test link failed' }
Set-Location $root
& "$root\build\bin\test_glass_lamps_material.exe"
if ($LASTEXITCODE -ne 0) { throw 'Material regression failed' }
Set-Location "$root\build"
foreach ($name in @('character.frag','terrain.frag','wmo.frag','m2.frag','rt_trace.comp')) {
    $stage = if ($name.EndsWith('.comp')) { '-fshader-stage=compute' } else { '-fshader-stage=fragment' }
    & 'C:\msys64\mingw64\bin\glslc.exe' $stage '-O' "$root\assets\shaders\$name.glsl" '-o' "$root\build\glass-lamps-shaders\$name.spv"
    if ($LASTEXITCODE -ne 0) { throw "Shader failed: $name" }
}
& 'C:\msys64\mingw64\bin\glslc.exe' '-fshader-stage=compute' '-O' '-DRT_HARDWARE' '--target-env=vulkan1.2' "$root\assets\shaders\rt_trace.comp.glsl" '-o' "$root\build\glass-lamps-shaders\rt_trace_hw.comp.spv"
if ($LASTEXITCODE -ne 0) { throw 'Hardware reflection shader failed' }
& 'C:\msys64\mingw64\bin\glslc.exe' '-fshader-stage=compute' '-O' "$root\tools\rt_lookup_probe.comp.glsl" '-o' "$root\build\glass-lamps-shaders\rt_lookup_probe.comp.spv"
if ($LASTEXITCODE -ne 0) { throw 'Lookup regression shader failed' }
Write-Output 'Material regressions and shader compilation passed'
