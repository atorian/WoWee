$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
$commands = Get-Content compile_commands.json -Raw | ConvertFrom-Json
$entry = $commands | Where-Object { $_.file -eq 'D:/games/WoWee/src/main.cpp' } | Select-Object -First 1
$command = [regex]::Replace($entry.command,'-o \S+', '-o glass-lamps-objects/rt_probe.cpp.obj')
$command = [regex]::Replace($command,'-c \S+', '-c D:\games\WoWee\tools\rt_probe.cpp')
& cmd.exe '/d' '/s' '/c' $command
if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed' }
& 'C:\msys64\mingw64\bin\c++.exe' '-O3' '-DNDEBUG' '-flto=4' '-fno-fat-lto-objects' '-Wno-odr' '@glass-lamps-probe.rsp' '-o' 'bin/rt-glass-lamps-probe.exe'
if ($LASTEXITCODE -ne 0) { throw 'Probe link failed' }
Write-Output 'Probe linked'
