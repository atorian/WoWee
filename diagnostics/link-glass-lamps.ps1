param([switch]$ProbeOnly)
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
$root = 'D:\games\WoWee'
Set-Location "$root\build"
$ninja = Get-Content build.ninja -Raw
$line = @($ninja -split "`n" | Where-Object { $_.StartsWith('build bin/wowee.exe:') })[0]
$objects = [regex]::Matches($line, 'CMakeFiles/wowee\.dir/[^ ]+\.obj') | ForEach-Object { $_.Value }
$block = $ninja.Substring($ninja.IndexOf($line))
$libs = [regex]::Match($block, '(?m)^  LINK_LIBRARIES = (.+)$').Groups[1].Value.Trim()
$clientObjects = @()
$probeObjects = @()
foreach ($object in $objects) {
    $source = $object
    $rel = $object.Replace('CMakeFiles/wowee.dir/','')
    $changed = "glass-lamps-objects/$rel"
    if (Test-Path $changed) { $source = $changed }
    $snapshot = "glass-lamps-link/$rel"
    New-Item -ItemType Directory -Force -Path (Split-Path $snapshot) | Out-Null
    $stable = $false
    for ($attempt=0; $attempt -lt 30; ++$attempt) {
        try {
            $before = Get-Item $source
            $stamp = $before.LastWriteTimeUtc.Ticks
            $bytes = [System.IO.File]::ReadAllBytes((Join-Path "$root\build" $source))
            $after = Get-Item $source
            if ($bytes.Length -gt 128 -and $bytes[0] -eq 100 -and $bytes[1] -eq 134 -and $after.LastWriteTimeUtc.Ticks -eq $stamp -and $after.Length -eq $bytes.Length) { $stable = $true; break }
        } catch { }
        Start-Sleep -Milliseconds 300
    }
    if (-not $stable) { throw "Unstable object: $source" }
    [System.IO.File]::WriteAllBytes((Join-Path "$root\build" $snapshot), $bytes)
    $clientObjects += ('"' + $snapshot + '"')
    if ($rel -ne 'src/main.cpp.obj') { $probeObjects += ('"' + $snapshot + '"') }
}
$probeObjects += '"glass-lamps-objects/rt_probe.cpp.obj"'
[System.IO.File]::WriteAllText("$root\build\glass-lamps-client.rsp", ($clientObjects -join "`n") + "`n" + $libs + "`n")
[System.IO.File]::WriteAllText("$root\build\glass-lamps-probe.rsp", ($probeObjects -join "`n") + "`n" + $libs + "`n")
$targets = @('probe')
if (-not $ProbeOnly) { $targets += 'client' }
foreach ($target in $targets) {
    $output = if ($target -eq 'probe') { 'bin/rt-glass-lamps-probe.exe' } else { 'bin/wowee-glass-lamps-validated.exe' }
    & 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '-flto=4' '-fno-fat-lto-objects' '-Wno-odr' "@glass-lamps-$target.rsp" '-o' $output '-Wl,--major-image-version,0,--minor-image-version,0'
    if ($LASTEXITCODE -ne 0) { throw "Link failed: $target" }
    Write-Output "Linked $output from stable object snapshots"
}
