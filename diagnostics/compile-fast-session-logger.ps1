$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
$commands = Get-Content compile_commands.json -Raw | ConvertFrom-Json
$entry = $commands | Where-Object { $_.file.EndsWith('/src/core/logger.cpp') -and $_.output -match '/wowee_test_common\.dir/' } | Select-Object -First 1
if (-not $entry) { throw 'Missing common logger command' }
$object = 'fast-session-objects/' + $entry.output.Replace('D:/games/WoWee/build/','')
New-Item -ItemType Directory -Force -Path (Split-Path $object) | Out-Null
$command = [regex]::Replace($entry.command, '-o \S+', "-o $object")
& cmd.exe '/d' '/s' '/c' $command
if ($LASTEXITCODE -ne 0) { throw 'Logger compile failed' }
& 'D:\games\WoWee\diagnostics\link-fast-session.ps1'
