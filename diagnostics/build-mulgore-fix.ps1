$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
$root = 'D:\games\WoWee'
$build = "$root\build"
Set-Location $build
$commands = Get-Content "$build\compile_commands.json" -Raw | ConvertFrom-Json
$files = @('src/core/application.cpp','src/pipeline/dbc_loader.cpp','src/rendering/terrain_renderer.cpp','src/rendering/wmo_renderer.cpp','src/rendering/m2_renderer_render.cpp')
$jobs = @()
foreach ($file in $files) {
    $entry = $commands | Where-Object { $_.file -eq "$($root.Replace('\','/'))/$file" -and $_.output -match '/wowee\.dir/' } | Select-Object -First 1
    if (-not $entry) { throw "No compile command for $file" }
    $object = "mulgore-objects/$file.obj"
    $source = "mulgore-source/$file"
    New-Item -ItemType Directory -Force -Path (Split-Path $object),(Split-Path $source) | Out-Null
    Copy-Item "$root/$file" $source -Force
    $command = [regex]::Replace($entry.command, '-o \S+', "-o $object")
    $command = [regex]::Replace($command, '-c \S+', "-c $source")
    $command += " -I$($root.Replace('\','/'))/src/rendering"
    $label = [System.IO.Path]::GetFileNameWithoutExtension($file)
    $process = Start-Process cmd.exe -ArgumentList @('/d','/s','/c',"`"$command`"") -PassThru -WindowStyle Hidden -RedirectStandardOutput "$build\mulgore-$label-build.out" -RedirectStandardError "$build\mulgore-$label-build.err"
    $null = $process.Handle
    $jobs += @{Process=$process; Label=$label}
}
foreach ($job in $jobs) {
    $job.Process.WaitForExit()
    $job.Process.Refresh()
    if ($job.Process.ExitCode -ne 0) { Get-Content "$build\mulgore-$($job.Label)-build.err"; throw "Compilation failed: $($job.Label)" }
    Write-Output "Compiled $($job.Label)"
}
$ninja = [System.IO.File]::ReadAllText("$build\build.ninja")
$line = @($ninja -split "`n" | Where-Object { $_.StartsWith('build bin/wowee.exe:') })[0]
$objects = [regex]::Matches(($line -split ' \|')[0], 'CMakeFiles/wowee\.dir/[^ ]+\.obj')
$libs = [regex]::Match($ninja.Substring($ninja.IndexOf($line)), '(?m)^  LINK_LIBRARIES = (.+)$').Groups[1].Value.Trim()
$response = @()
foreach ($match in $objects) {
    $relative = $match.Value
    $file = $relative.Replace('CMakeFiles/wowee.dir/', '').Replace('.obj','')
    $source = if ($files -contains $file) { "$build\mulgore-objects/$file.obj" } else { "$build/$relative" }
    $target = "mulgore-snapshot/$relative"
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item $source $target -Force
    if ((Get-FileHash $source).Hash -ne (Get-FileHash $target).Hash) { throw "Object changed during snapshot: $relative" }
    $response += '"' + $target + '"'
}
[System.IO.File]::WriteAllText("$build\mulgore-snapshot.rsp", ($response -join "`n") + "`n" + $libs + "`n")
Write-Output "Linking immutable snapshot of $($objects.Count) objects"
& 'C:\msys64\mingw64\bin\c++.exe' -O3 -DNDEBUG -flto=4 -fno-fat-lto-objects -Wno-odr '@mulgore-snapshot.rsp' -o 'bin\wowee-mulgore-fix.exe' '-Wl,--out-implib,lib\libwowee-mulgore.dll.a' '-Wl,--major-image-version,0,--minor-image-version,0'
if ($LASTEXITCODE -ne 0) { throw 'Snapshot link failed' }
Write-Output 'Mulgore client linked; existing runtime shaders preserved'
