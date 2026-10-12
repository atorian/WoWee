$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
$root = 'D:\games\WoWee'
$build = "$root\build"
Set-Location $build
$commands = Get-Content compile_commands.json -Raw | ConvertFrom-Json
$files = @('src/game/game_handler.cpp','src/game/warden_handler.cpp','src/game/server_extension.cpp','src/game/server_protocol.cpp')
$entries = @($commands | Where-Object { ($files -contains $_.file.Replace('D:/games/WoWee/','')) -and $_.output -match '/wowee\.dir/' })
$entries += @($commands | Where-Object { $_.output -match '/test_server_extension\.dir/' })
$entries += @($commands | Where-Object { $_.output -match '/test_warden_challenge\.dir/' })
$entries += @($commands | Where-Object { $_.output -match '/wowee_test_common\.dir/' })
$jobs = @()
$replacements = @{}
foreach ($entry in $entries) {
    $relative = $entry.output.Replace('D:/games/WoWee/build/','')
    $object = "fast-session-objects/$relative"
    New-Item -ItemType Directory -Force -Path (Split-Path $object) | Out-Null
    $command = [regex]::Replace($entry.command, '-o \S+', "-o $object")
    $label = [System.IO.Path]::GetFileNameWithoutExtension($entry.file) + '-' + $jobs.Count
    $process = Start-Process cmd.exe -ArgumentList @('/d','/s','/c',"`"$command`"") -PassThru -WindowStyle Hidden -RedirectStandardOutput "early-character-list-$label.out" -RedirectStandardError "early-character-list-$label.err"
    $null = $process.Handle
    $jobs += @{Process=$process; Label=$label}
    $replacements[$relative.Replace('\','/')] = $object
}
foreach ($job in $jobs) {
    $job.Process.WaitForExit()
    $job.Process.Refresh()
    if ($job.Process.ExitCode -ne 0) { Get-Content "early-character-list-$($job.Label).err"; throw "Compilation failed: $($job.Label)" }
    Write-Output "Compiled $($job.Label)"
}
$ninja = Get-Content build.ninja -Raw
$testLine = @($ninja -split "`n" | Where-Object { $_.StartsWith('build bin/test_server_extension.exe:') })[0]
$testObjects = [regex]::Matches(($testLine -split ' \|')[0], 'tests/CMakeFiles/test_server_extension\.dir/[^ ]+\.obj') | ForEach-Object { $replacements[$_.Value] }
if ($testObjects.Count -ne 4 -or @($testObjects | Where-Object { -not $_ }).Count) { throw 'Missing isolated test objects' }
$testLibs = [regex]::Match($ninja.Substring($ninja.IndexOf($testLine)), '(?m)^  LINK_LIBRARIES = (.+)$').Groups[1].Value.Trim()
[System.IO.File]::WriteAllText("$build\fast-session-tests.rsp", ($testObjects -join "`n") + "`n" + $testLibs + "`n")
& 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '@fast-session-tests.rsp' '-o' 'bin/test_fast_session_protocol.exe'
if ($LASTEXITCODE -ne 0) { throw 'Protocol test link failed' }
& "$build\bin\test_fast_session_protocol.exe"
if ($LASTEXITCODE -ne 0) { throw 'Protocol/extension regression failed' }
$wardenLine = @($ninja -split "`n" | Where-Object { $_.StartsWith('build bin/test_warden_challenge.exe:') })[0]
$wardenObjects = [regex]::Matches(($wardenLine -split ' \|')[0], '[^ ]+\.obj') | ForEach-Object {
    if ($replacements.ContainsKey($_.Value)) { $replacements[$_.Value] }
    else {
        $copy = 'fast-session-objects/' + $_.Value
        New-Item -ItemType Directory -Force -Path (Split-Path $copy) | Out-Null
        Copy-Item $_.Value $copy -Force
        $copy
    }
}
if (@($wardenObjects | Where-Object { -not $_ }).Count) { throw 'Missing isolated Warden objects' }
$wardenLibs = [regex]::Match($ninja.Substring($ninja.IndexOf($wardenLine)), '(?m)^  LINK_LIBRARIES = (.+)$').Groups[1].Value.Trim()
[System.IO.File]::WriteAllText("$build\fast-session-warden.rsp", ($wardenObjects -join "`n") + "`n" + $wardenLibs + "`n")
& 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '@fast-session-warden.rsp' '-o' 'bin/test_fast_session_warden.exe'
if ($LASTEXITCODE -ne 0) { throw 'Warden test link failed' }
& "$build\bin\test_fast_session_warden.exe"
if ($LASTEXITCODE -ne 0) { throw 'Warden regression failed' }
$baseline = Get-Content early-character-list-client.rsp
$client = @()
foreach ($line in $baseline) {
    if ($line -match '^"early-character-list-link/(.+)"$') {
        $relative = 'CMakeFiles/wowee.dir/' + $Matches[1]
        $source = if ($replacements.ContainsKey($relative)) { $replacements[$relative] } else { $line.Trim('"') }
        $target = 'fast-session-link/' + $Matches[1]
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item $source $target -Force
        if ((Get-FileHash $source).Hash -ne (Get-FileHash $target).Hash) { throw "Object changed during snapshot: $source" }
        $client += '"' + $target + '"'
    } else { $client += $line }
}
[System.IO.File]::WriteAllText("$build\fast-session-client.rsp", ($client -join "`n") + "`n")
& 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '-flto=4' '-fno-fat-lto-objects' '-Wno-odr' '@fast-session-client.rsp' '-o' 'bin/wowee-fast-session.exe' '-Wl,--major-image-version,0,--minor-image-version,0'
if ($LASTEXITCODE -ne 0) { throw 'Client link failed' }
Write-Output 'Fast session client linked from isolated objects; original executable preserved'
