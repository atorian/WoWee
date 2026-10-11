$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
& 'C:\msys64\mingw64\bin\ninja.exe' -f rt-halo-probe.ninja -j 4 bin/rt-halo-probe.exe
exit $LASTEXITCODE
